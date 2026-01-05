#pragma once

#include <variant>
#include <numeric>
#include <any>
#include <cassert>
#include <string_view>
#include <sstream>
#include <optional>
#include <unordered_map>
#include <math.h>
#include <unordered_set>
#include <list>
#include <ctype.h>
#include <vector>
#include <stack>
#include <regex>
#include <type_traits>
#include <algorithm>
#include <fstream>
#include <set>
#include <functional>
#include <utility>
#include <memory>
#include <stdlib.h>
#include <string>
#include <map>
#include <iostream>
#include <sys/stat.h>
#include <filesystem>
#include <queue>
#include <cstdio>    // vsnprintf, SEEK_SET

// ================= BEGIN: Order-safe BBX PROBE =================
#ifndef BBX_PROBE_MIN_ONCE
#define BBX_PROBE_MIN_ONCE

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

namespace bbx_probe_min {

// --- safe logger (no bbx_diag dependency) ---
static inline void logf(const char* lvl, const char* event, const char* kvfmt, ...) {
    char buf[768];
    va_list ap; va_start(ap, kvfmt);
    int n = std::vsnprintf(buf, sizeof(buf), kvfmt, ap);
    va_end(ap);
    if (n < 0) return;
    std::fprintf(stderr, "ts=? lvl=%s role=client event=%s %.*s\n",
                 lvl, event, (int)std::min(n, (int)sizeof(buf)-1), buf);
}

// owning image of an address
static inline const char* image_of(const void* p, char* out, size_t n) {
    Dl_info info{};
    if (dladdr(p, &info) && info.dli_fname) std::snprintf(out, n, "%s", info.dli_fname);
    else std::snprintf(out, n, "unknown-image");
    return out;
}

// one marker per TU (lets us count how many TUs included this header)
static int tu_marker = 0;

// auto-log once PER TU at load time
__attribute__((constructor))
static void tu_seen_ctor() {
    char img[256];
    image_of((void*)&tu_marker, img, sizeof(img));
    logf("INFO","tu_seen","pid=%d file=%s tu_addr=%p tu_img=%s cxx=%ld",
         (int)getpid(), __FILE__, (void*)&tu_marker, img, (long)__cplusplus);
}

// snapshot of state/function addresses + owning images
static inline void bbx_probe_snapshot(const char* where,
                                      const void* state_addr, const char* state_name,
                                      const void* connect_once_addr,
                                      const void* send_blob_addr) {
    char is[256], ic[256], ib[256];
    image_of(state_addr,         is, sizeof(is));
    image_of(connect_once_addr,  ic, sizeof(ic));
    image_of(send_blob_addr,     ib, sizeof(ib));
    logf("INFO","bbx_snapshot",
         "where=%s pid=%d state=%s state_addr=%p state_img=%s "
         "connect_once_addr=%p connect_img=%s send_blob_addr=%p send_img=%s",
         where ? where : "?", (int)getpid(),
         state_name ? state_name : "?", state_addr, is,
         connect_once_addr, ic, send_blob_addr, ib);
}

} // namespace bbx_probe_min
#endif
// ================= END: Order-safe BBX PROBE =================

#if defined(__unix__) || defined(__APPLE__)
  #include <unistd.h>
  #include <fcntl.h>
#endif

#ifdef COMPILE_TEST

#include <cstdarg>   // va_list, va_start, va_end
#include <cstdlib>   // For size_t
#include <cstring>   // For strlen if needed
#include <string>
#include <mutex>
#include <fstream>
#include <vector>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cerrno>
#include <chrono>

#include "black_box_api.h"
#include "project_config.h"

// ======= client diag (very small, one-line events) =======
#if defined(__unix__) || defined(__APPLE__)
#include <sys/time.h>
#include <atomic>
#include <cstdarg>
#include <cerrno>

namespace bbx_diag {
inline int logfd = -1;
inline std::atomic<uint64_t> msgs_total{0}, bytes_total{0}, send_errors{0}, reconnects{0};
inline thread_local bool logged_partial = false;

inline const char* iso8601(char* buf, size_t n) {
    struct timeval tv; gettimeofday(&tv, nullptr);
    time_t t = tv.tv_sec; struct tm tm; gmtime_r(&t, &tm);
    int ms = static_cast<int>(tv.tv_usec / 1000);
    std::snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return buf;
}

inline void init() {
    if (logfd >= 0) return;
    const char* p = ::getenv("BLACKBOX_CLIENT_DIAG");
    const char* path = (p && *p) ? p : "/tmp/bbx_client.diag.log";
    logfd = ::open(path, O_CREAT|O_APPEND|O_WRONLY|O_CLOEXEC, 0644);
}

inline void logf(const char* lvl, const char* ev, const char* kvfmt, ...) {
    init(); if (logfd < 0) return;
    char ts[40]; iso8601(ts, sizeof(ts));
    char body[512]; va_list ap; va_start(ap, kvfmt);
    int n = std::vsnprintf(body, sizeof(body), kvfmt, ap); va_end(ap);
    if (n < 0) return;
    char line[800];
    int m = std::snprintf(line, sizeof(line), "ts=%s lvl=%s role=client event=%s %.*s\n",
                          ts, lvl, ev, (int)std::min<size_t>(sizeof(body)-1, (size_t)n), body);
    if (m > 0) (void)::write(logfd, line, m);
}
 
inline void summary(int last_errno) {
    logf("INFO","session_summary",
         "msgs_total=%llu bytes_total=%llu send_errors=%llu reconnects=%llu last_errno=%d",
         (unsigned long long)msgs_total.load(),
         (unsigned long long)bytes_total.load(),
         (unsigned long long)send_errors.load(),
         (unsigned long long)reconnects.load(), last_errno);
}

} // namespace bbx_diag
#endif
// ======= end client diag =======


namespace black_box_api {

// Single instance across all translation units (C++17 inline variables)
inline int g_sock = -1;

// --- small helper to stringify errno (macOS-compatible) ---
inline const char* err_str(int e, char* buf, size_t n) {
#if defined(__APPLE__) || (_POSIX_C_SOURCE >= 200112L)
    if (strerror_r(e, buf, n) == 0) return buf;          // XSI strerror_r
    std::snprintf(buf, n, "errno=%d", e); return buf;
#else
    const char* s = std::strerror(e);
    if (!s) { std::snprintf(buf, n, "errno=%d", e); return buf; }
    std::snprintf(buf, n, "%s", s); return buf;
#endif
}

inline void send_blob(uint16_t type, const void* data, uint32_t len);

// ---- connect_once with anomaly logging (no mutex; single-threaded) ----
inline bool connect_once() {
    if (g_sock >= 0) return true;

    // create socket
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
#if defined(__unix__) || defined(__APPLE__)
        char ebuf[128];
        bbx_diag::reconnects.fetch_add(1);
        bbx_diag::logf("ERROR","connect_fail","stage=socket errno=%d err=%s", errno, err_str(errno, ebuf, sizeof(ebuf)));
#endif
        return false;
    }

    // CLOEXEC
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    // Disable Nagle (latency)
    int one = 1;
    //(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // macOS: avoid SIGPIPE on send()
#if defined(__APPLE__)
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    // connect to 127.0.0.1:BLACK_BOX_PORT
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);     // 127.0.0.1
    sa.sin_port        = htons(BLACK_BOX_PORT);      // define elsewhere

    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
#if defined(__unix__) || defined(__APPLE__)
        char ebuf[128];
        bbx_diag::reconnects.fetch_add(1);
        bbx_diag::logf("ERROR","connect_fail","stage=connect port=%u errno=%d err=%s",
                       (unsigned)BLACK_BOX_PORT, errno, err_str(errno, ebuf, sizeof(ebuf)));
#endif
        ::close(fd);
        return false;
    }
    
    {
        // 1) Make the kernel send buffer roomy (lets you burst without stalling)
        //int sndbuf = 4 << 20; // 1 MiB; tune 256 KiB..4 MiB
        //(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        // 2) Bound how long a blocking send can sleep
        //timeval tv { 0, 5000 }; // 5 ms timeout; try 1–10ms
        //(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // 3) (Optional) reduce Nagle latency for small batches
        //int one = 1;
        //(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // 4) Never send SIGPIPE on EPIPE
        // (either MSG_NOSIGNAL per-call or global SO_NOSIGPIPE on macOS)
        //int nosig = 1;
        //(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
    }

    // success
    g_sock = fd;
    
    static bool s_snap = false;
    if (!s_snap) {
        bbx_probe_min::bbx_probe_snapshot("after_connect_ok",
                                          (const void*)&g_sock, "inline_var:g_sock",
                                          (const void*)(void*)&connect_once,
                                          (const void*)(void*)&send_blob);
        s_snap = true;
    }
    
#if defined(__unix__) || defined(__APPLE__)
    bbx_diag::logf("INFO","connect_ok","pid=%d port=%u sock=%d",
                   (int)getpid(), (unsigned)BLACK_BOX_PORT, fd);
#endif
    return true;
}


inline bool sendmsg_all(int fd, struct iovec* iov, int iovcnt) {
    size_t total = 0;
    for (int i = 0; i < iovcnt; ++i) total += iov[i].iov_len;
    size_t sent = 0;
    while (sent < total) {
        struct msghdr msg{};
        msg.msg_iov = iov; msg.msg_iovlen = iovcnt;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = ::sendmsg(fd, &msg, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // consider backoff/sleep
            return false;
        }
        // advance iov by n
        size_t left = n;
        for (int i = 0; i < iovcnt && left; ++i) {
            size_t used = std::min<size_t>(iov[i].iov_len, left);
            iov[i].iov_base = static_cast<char*>(iov[i].iov_base) + used;
            iov[i].iov_len  -= used;
            left -= used;
        }
        sent += n;
    }
    return true;
}

inline void send_blob(uint16_t type, const void* data, uint32_t len) {
    if (!connect_once()) {
#if defined(__unix__) || defined(__APPLE__)
        char ebuf[128];
        bbx_diag::reconnects.fetch_add(1);
        bbx_diag::logf("ERROR","connect_fail","port=%u errno=%d err=%s",
                       (unsigned)BLACK_BOX_PORT, errno, err_str(errno, ebuf, sizeof(ebuf)));
#endif
        return;
    }
    
    int fd = g_sock;
    MsgHdr h{len, type, 0};

    struct iovec iov[2] = {
        { &h, sizeof(h) },
        { const_cast<void*>(data), (size_t)len }
    };
    int iovcnt = len ? 2 : 1;

    if (!sendmsg_all(fd, iov, iovcnt)) {
        if (fd >= 0) ::close(fd);
        g_sock = -1;
        return;
    }
    
#if defined(__unix__) || defined(__APPLE__)
    // Success counters (used by the one-line session_summary)
    bbx_diag::msgs_total.fetch_add(1);
    bbx_diag::bytes_total.fetch_add(len);
#endif
}

inline void trace_append(std::string_view sv) {
    send_blob(MSG_TRACE_APPEND, sv.data(), static_cast<uint32_t>(sv.size()));
}

inline void stdout_append(std::string_view sv) {
    send_blob(MSG_STDOUT_APPEND, sv.data(), static_cast<uint32_t>(sv.size()));
}

inline void memo_replace(std::string_view sv) {
    send_blob(MSG_MEMO_REPLACE, sv.data(), static_cast<uint32_t>(sv.size()));
}

// Stack file controls (optional but recommended for big wins)
inline void stack_reopen(const std::string& path, bool truncate) {
    std::vector<char> payload;
    payload.reserve(1 + path.size());
    payload.push_back(truncate ? 1 : 0);
    payload.insert(payload.end(), path.begin(), path.end());
    send_blob(MSG_STACK_REOPEN, payload.data(), (uint32_t)payload.size());
}
 
inline void stack_remove() {
    send_blob(MSG_STACK_REMOVE, nullptr, 0);
}

inline void stack_write(std::string_view sv) {
    send_blob(MSG_STACK_WRITE, sv.data(), (uint32_t)sv.size());
}

inline void trace_and_stack(std::string_view sv) {
    send_blob(MSG_TRACE_AND_STACK, sv.data(), (uint32_t)sv.size());
}

inline void save_breakpoint(const std::string& bp_name)
{
    send_blob(MSG_SAVE_BREAKPOINT, bp_name.data(), (uint32_t)bp_name.size());
}

} // namespace black_box_api

// BEGIN format-security suppression
#if defined(__clang__)
// Clang
  #define PUSH_FMT_SECURE()  _Pragma("clang diagnostic push")              \
                              _Pragma("clang diagnostic ignored \"-Wformat-security\"")
  #define POP_FMT_SECURE()   _Pragma("clang diagnostic pop")

#elif defined(__GNUC__)
// GCC
  #define PUSH_FMT_SECURE()  _Pragma("GCC diagnostic push")                \
                              _Pragma("GCC diagnostic ignored \"-Wformat-security\"")
  #define POP_FMT_SECURE()   _Pragma("GCC diagnostic pop")

#elif defined(_MSC_VER)
// MSVC
  // MSVC doesn’t warn on non-literal formats by default, but if you need to
  // disable a specific warning (e.g. C4477 for format-mismatches), change 4477
  #define PUSH_FMT_SECURE()  __pragma(warning(push))                      \
                              __pragma(warning(disable:4477))
  #define POP_FMT_SECURE()   __pragma(warning(pop))

#else
  // other compilers: no-ops
  #define PUSH_FMT_SECURE()
  #define POP_FMT_SECURE()
#endif
// END format-security suppression

// ------------------------------------------------------------
// Configuration
// ------------------------------------------------------------

// Single-message limit: if one logging call writes more than this,
// it is truncated and "[TRUNCATED_SINGLE_MESSAGE]" is appended.
static const std::size_t MAX_SINGLE_MESSAGE = 8192;

// High-water mark: if file exceeds 25 MB, we do a head+tail rewrite.
static const std::size_t HIGH_WATER_MARK   = 25ULL * 1024ULL * 1024ULL;

// Low-water mark: after truncation, the file is ~15 MB (10 MB head + 5 MB tail).
static const std::size_t LOW_WATER_MARK    = 15ULL * 1024ULL * 1024ULL;

// Head/Tail sizes for the rewrite
static const std::size_t HEAD_KEEP_SIZE    = 10ULL * 1024ULL * 1024ULL;
static const std::size_t TAIL_KEEP_SIZE    =  5ULL * 1024ULL * 1024ULL;

// Where we store logs
static const char*       LOG_FILENAME      = "stdout.log";

namespace trace
{
    static int getHitCount();
}

// ------------------------------------------------------------
// LogFileManager: a singleton-like class to manage the file
// ------------------------------------------------------------
class LogFileManager
{
public:
    // Provide a global instance you can call anywhere
    static LogFileManager& instance()
    {
        static LogFileManager gInstance;
        return gInstance;
    }

    // Replace your current writeLog method with this version
    template<typename... Args>
    void writeLog(const char* func, int line, const char* fmt, Args&&... args)
    {
        // format first (no lock)
        PUSH_FMT_SECURE();
        std::string user = formatString(fmt, std::forward<Args>(args)...);
        POP_FMT_SECURE();

        std::string msg;
        msg.reserve(MAX_SINGLE_MESSAGE + 128);
        msg += func;
        msg += ":"; msg += std::to_string(trace::getHitCount());
        msg += " ln "; msg += std::to_string(line);
        msg += " - ";
        msg += user;
        msg.push_back('\n');

        black_box_api::stdout_append(msg);
    }
    
    inline std::string formatString(const char* format) {
        thread_local std::vector<char> buf(256);
        for (;;) {
            int n = std::snprintf(buf.data(), buf.size(), "%s", format); // treat as data
            if (n < 0) return "FORMAT_ERROR";
            if (size_t(n) < buf.size()) return std::string(buf.data(), size_t(n));
            buf.resize(size_t(n) + 1);
        }
    }

    template<typename... Args>
    #if defined(__clang__) || defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
    #endif
    std::string formatString(const char* format, Args&&... args) {
        thread_local std::vector<char> buf(256);
        for (;;) {
            int n = std::snprintf(buf.data(), buf.size(), format, std::forward<Args>(args)...);
            if (n < 0) return "FORMAT_ERROR";
            if (size_t(n) < buf.size()) return std::string(buf.data(), size_t(n));
            buf.resize(size_t(n) + 1);
        }
    }


private:
    // Constructor: open the file for read+write at end (or create if missing).
    LogFileManager()
    {
        std::filesystem::remove(LOG_FILENAME);
        
        openOrCreateFile();
    }

    // Destructor: close file if open.
    ~LogFileManager()
    {
        if (file_.is_open()) {
            file_.close();
        }
    }

    // We forbid copy or assign
    LogFileManager(const LogFileManager&) = delete;
    LogFileManager& operator=(const LogFileManager&) = delete;

    // Called when we need to open the file if not already open,
    // or create it if it doesn't exist.
    void openOrCreateFile()
    {
        if (!file_.is_open())
        {
            // switch to append-only write; no reads needed
            file_.open(LOG_FILENAME, std::ios::out | std::ios::app | std::ios::binary);
            if (file_.rdbuf()) file_.rdbuf()->pubsetbuf(io_buf_.data(), io_buf_.size());

            struct stat st{};
            size_now_ = (::stat(LOG_FILENAME, &st) == 0) ? (std::uint64_t)st.st_size : 0ULL;
        }
    }

private:
    std::mutex    mutex_;
    std::fstream  file_;
    
    std::uint64_t size_now_ = 0;
    std::array<char, 64 * 1024> io_buf_{};   // larger stream buffer
};

// ------------------------------------------------------------
// Public macro: PRINT_TEST
//   Example: PRINT_TEST("Hello %s, value=%d", name, val);
// ------------------------------------------------------------
#define PRINT_TEST(format, ...) \
    do { \
        LogFileManager::instance().writeLog(__func__, __LINE__, format, ##__VA_ARGS__); \
    } while(0)
#else //COMPILE_TEST
#define PRINT_TEST(format, ...)
#endif //COMPILE_TEST

// ------------------------------------------------------------
// INSTRUMENTATION PRINTERS
// ------------------------------------------------------------

// Configuration for how deeply to recurse, how many elements to print, etc.
struct PrintConfig {
    std::size_t maxHitCount = 3;
    std::size_t maxDepth = 2;       // Maximum recursion depth
    std::size_t maxElements = 4;    // Max number of elements per container
    std::size_t maxMembers = 16;     // Max number of members per struct/enum
    std::size_t maxStringSize = 64; // Max string length to display
};

// C++14 does not have std::void_t, so define your own:
template <typename... Ts> struct make_void { typedef void type; };
template <typename... Ts> using void_t = typename make_void<Ts...>::type;

// Detect if a type T is "iterable" (has begin() and end())
template <typename T, typename = void>
struct is_iterable : std::false_type {};

template <typename T>
struct is_iterable<T, void_t<decltype(std::declval<T>().begin()),
                             decltype(std::declval<T>().end())>>
    : std::true_type
{
};

template <typename T>
static constexpr bool is_iterable_v = is_iterable<T>::value;

//
// Trait to detect char pointers (char* or const char*)
//
template <typename T>
struct is_char_pointer : std::false_type {};

template <>
struct is_char_pointer<char*> : std::true_type {};

template <>
struct is_char_pointer<const char*> : std::true_type {};

// A helper trait to test if operator<< is available for type T.
template<typename T, typename = void>
struct is_streamable : std::false_type {};

template<typename T>
struct is_streamable<T, std::void_t<
    decltype(std::declval<std::ostream&>() << std::declval<T>())
>> : std::true_type {};

template<typename T>
constexpr bool is_streamable_v = is_streamable<T>::value;

// Forward declarations with appropriate constraints
// 1. For scalar types
template <typename T,
          typename std::enable_if<
              !std::is_same<T, bool>::value &&
              !is_iterable_v<T> &&
              !std::is_same<T, std::string>::value &&
              !std::is_pointer<T>::value &&
              is_streamable_v<T>,
              int
          >::type = 0>
void printValue(std::ostream& os, const T& value, std::size_t depth, const PrintConfig& cfg);

// 3. For containers
template <typename Container, typename std::enable_if<is_iterable_v<Container> && !std::is_same<Container, std::string>::value, int>::type = 0>
void printValue(std::ostream& os, const Container& cont, std::size_t depth, const PrintConfig& cfg);

// 4. For shared_ptr
template <typename T>
void printValue(std::ostream& os, const std::shared_ptr<T>& ptr, std::size_t depth, const PrintConfig& cfg);

// --- helpers (place once in trace namespace, before overloads) ---
inline void write_escaped(std::ostream& os, const char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        switch (c) {
            case '\\': os << "\\\\"; break;
            case '\"': os << "\\\""; break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (c >= 32 && c <= 126) os << static_cast<char>(c);
                else { // hex-escape control bytes
                    static constexpr char HEX[] = "0123456789ABCDEF";
                    os << "\\x" << HEX[(c>>4)&0xF] << HEX[c&0xF];
                }
        }
    }
}
inline std::size_t safe_strlen_cap(const char* s, std::size_t cap) {
    if (!s) return 0;
    std::size_t n = 0;
    for (; n < cap && s[n] != '\0'; ++n) {}
    return n; // returns cap if not found, preventing overrun
}

// --- Specific overloads: ALWAYS treat as text, but cap length ---

// const char*
inline void printValue(std::ostream& os,
                       const char* s,
                       std::size_t /*depth*/,
                       const PrintConfig& cfg)
{
    if (!s) { os << "nullptr"; return; }
    const std::size_t cap = cfg.maxStringSize ? cfg.maxStringSize : 1024; // hard cap fallback
    const std::size_t len = safe_strlen_cap(s, cap + 1); // +1 to detect truncation
    os << '"';
    write_escaped(os, s, (len > cap ? cap : len));
    if (len > cap) os << "...";
    os << '"';
}

// char[N]
template <std::size_t N>
inline void printValue(std::ostream& os,
                       const char (&arr)[N],
                       std::size_t /*depth*/,
                       const PrintConfig& cfg)
{
    const std::size_t cap = cfg.maxStringSize ? cfg.maxStringSize : 1024;
    std::size_t len = 0;
    while (len < N && arr[len] != '\0') ++len;  // bounded by N
    os << '"';
    write_escaped(os, arr, (len > cap ? cap : len));
    if (len > cap) os << "...";
    os << '"';
}

template <typename T,
          typename = std::enable_if_t<
              std::is_pointer_v<T> &&
              !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>
          >
>
inline void printValue(std::ostream& os, T ptr, std::size_t /*depth*/, const PrintConfig& /*cfg*/)
{
    using Pointee = std::remove_pointer_t<T>;
    if constexpr (std::is_function_v<Pointee>) {
        os << "[[function pointer]]";
    } else {
        if (!ptr) { os << "[[null pointer]]"; return; }
        os << "[[pointer: " << static_cast<const void*>(ptr) << "]]";
    }
}

// -------------------------------------------------------------------------- //
// Print Scalar Types (fall-through)
//    (excluded: std::string, pointers, iterables, etc.)
// -------------------------------------------------------------------------- //
template <typename T,
          typename std::enable_if<
              !std::is_same<T, bool>::value &&
              !is_iterable_v<T> &&
              !std::is_same<T, std::string>::value &&
              !std::is_pointer<T>::value &&
              is_streamable_v<T>,
              int
          >::type>
void printValue(std::ostream& os, const T& value, std::size_t depth, const PrintConfig& cfg)
{
    os << value; // Just print directly
}

// -------------------------------------------------------------------------- //
// Print std::string with size limit
// -------------------------------------------------------------------------- //
inline void printValue(std::ostream& os,
                       const std::string& value,
                       std::size_t /*depth*/,
                       const PrintConfig& cfg)
{
    if(value.empty())
    {
        os << "[[empty string]]";
    }
    else if (value.size() > cfg.maxStringSize) {
        os << "\"" << value.substr(0, cfg.maxStringSize) << "...\"";
    } else {
        os << "\"" << value << "\"";
    }
}

inline void printValue(std::ostream& os,
                       bool value,
                       std::size_t /*depth*/,
                       const PrintConfig& /*cfg*/)
{
    os << (value ? "true" : "false");
}

// -------------------------------------------------------------------------- //
// Print shared_ptr<T> by printing *ptr if non-null
// -------------------------------------------------------------------------- //
template <typename T>
void printValue(std::ostream& os,
                const std::shared_ptr<T>& ptr,
                std::size_t depth,
                const PrintConfig& cfg)
{
    if (!ptr) {
        os << "[[null shared_ptr]]";
        return;
    }
    printValue(os, *ptr, depth, cfg);
}

// -------------------------------------------------------------------------- //
// Print std::pair<K, V> so map containers (which store pairs) look nice
// -------------------------------------------------------------------------- //
template <typename K, typename V>
void printValue(std::ostream& os,
                const std::pair<K, V>& p,
                std::size_t depth,
                const PrintConfig& cfg)
{
    os << "(";
    printValue(os, p.first, depth + 1, cfg);
    os << " -> ";
    printValue(os, p.second, depth + 1, cfg);
    os << ")";
}

// -------------------------------------------------------------------------- //
// Print "iterable" containers with begin()/end() (e.g. vector, list, set)
// -------------------------------------------------------------------------- //
template <typename Container, typename std::enable_if< is_iterable_v<Container> && !std::is_same<Container, std::string>::value, int>::type>
void printValue(std::ostream& os,
                const Container& cont,
                std::size_t depth,
                const PrintConfig& cfg)
{
    // If we've reached or exceeded max depth, just show {...}
    if (depth >= cfg.maxDepth) {
        os << "{...}";
        return;
    }

    os << "{size=" << cont.size() << ", ";
    std::size_t count = 0;
    for (auto it = cont.begin(); it != cont.end(); ++it) {
        if (count > 0) {
            os << ", ";
        }
        if (count == cfg.maxElements) {
            os << "...";
            break;
        }
        printValue(os, *it, depth + 1, cfg);
        ++count;
    }
    os << "}";
}

// -------------------------------------------------------------------------- //
// Print std::stack<T> and std::queue<T> (no begin()/end() methods)
//    We'll pass by value to avoid mutating the caller's container.
// -------------------------------------------------------------------------- //
template <typename T>
void printValue(std::ostream& os,
                std::stack<T> st,
                std::size_t depth,
                const PrintConfig& cfg)
{
    if (depth >= cfg.maxDepth) {
        os << "[[stack: ...]]";
        return;
    }
    os << "[[stack: , size=" << st.size() << ", ";
    std::size_t count = 0;
    while (!st.empty()) {
        if (count > 0) {
            os << ", ";
        }
        if (count == cfg.maxElements) {
            os << "...";
            break;
        }
        printValue(os, st.top(), depth + 1, cfg);
        st.pop();
        ++count;
    }
    os << "]]";
}

template <typename T>
void printValue(std::ostream& os,
                std::queue<T> qu,
                std::size_t depth,
                const PrintConfig& cfg)
{
    if (depth >= cfg.maxDepth) {
        os << "[[queue: ...]]";
        return;
    }
    os << "[[queue: , size=" << qu.size() << ", ";
    std::size_t count = 0;
    while (!qu.empty()) {
        if (count > 0) {
            os << ", ";
        }
        if (count == cfg.maxElements) {
            os << "...";
            break;
        }
        printValue(os, qu.front(), depth + 1, cfg);
        qu.pop();
        ++count;
    }
    os << "]]";
}

// -------------------------------------------------------------------------- //
// Fallback overload for non-streamable types
// -------------------------------------------------------------------------- //
template <typename T,
          typename std::enable_if<!is_streamable_v<T> && !is_iterable_v<T> && !std::is_enum<T>::value, int>::type = 0>
void printValue(std::ostream& os,
                const T& /*value*/,
                std::size_t /*depth*/,
                const PrintConfig& /*cfg*/)
{
    os << "[[unsupported type]]";
}

// -------------------------------------------------------------------------- //
// Optional: A wrapper type + operator<< for convenience
// -------------------------------------------------------------------------- //
template <typename T>
struct Printer {
    const T& ref;
    PrintConfig cfg;
    std::size_t depth;
};

template <typename T>
Printer<T> make_printer(const T& x, const PrintConfig& cfg, std::size_t depth = 0) {
    return Printer<T>{x, cfg, depth};
}

// Regular version - constrained to NON-enum types
template <typename T, typename std::enable_if<!std::is_enum<T>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& os, const Printer<T>& wrapper)
{
    printValue(os, wrapper.ref, wrapper.depth, wrapper.cfg);
    return os;
}

// Specialized version for enum types
template <typename T, typename std::enable_if<std::is_enum<T>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& os, const Printer<T>& wrapper)
{
    // Explicitly cast to the enum type
    printValue(os, static_cast<T>(wrapper.ref), wrapper.depth, wrapper.cfg);
    return os;
}
// ------------------------------------------------------------
// TRACE LOG
// ------------------------------------------------------------

#define MAX_DETAILED_HIT_COUNT 3

namespace trace {

//===================================================================

inline void reset_breakpoint_dir() {
    std::error_code ec;

    // 1) If “breakpoint_logs/” already exists, remove it (and all of its contents).
    std::filesystem::remove_all("breakpoints/", ec);
    //    (ignore ec if you don’t care about failures here)

    // 2) Recreate an empty “breakpoint_logs/” directory.
    std::filesystem::create_directories("breakpoints/", ec);
    if (ec) {
        std::cerr << "Failed to (re)create breakpoints/: " << ec.message() << "\n";
    }
}

struct StackFrame
{
    std::string function;
    int invocation;
    bool hasBreakpoints;
    
    StackFrame(const std::string& _function, int _invocation):
    function(_function), invocation(_invocation), hasBreakpoints(false) {}
};

inline std::vector<StackFrame> frameStack;
inline std::vector<std::string> stackLabels;
inline std::string recursionLockFunction;
inline int recursionLockInvocation;
inline bool traceStarted = false;

// Opens (or reopens) the log file; default is "trace.txt".
inline void start(const std::string &filename = "trace.txt") {
    if(traceStarted) return;
    
    recursionLockFunction = std::string();
    recursionLockInvocation = -1;
    
    reset_breakpoint_dir();
    traceStarted = true;
}

static int getHitCount()
{
    if (!frameStack.empty())
    {
        return frameStack.back().invocation;
    }
    
    return -1;
}

inline bool inDeepRecursion() noexcept {
    return !recursionLockFunction.empty() && recursionLockInvocation >= 0;
}

struct LogWrapper {
    static std::string combined;

    template<typename T>
    LogWrapper& operator<<(const T& data) {
        bool traceAppend = (getHitCount() <= MAX_DETAILED_HIT_COUNT) && !inDeepRecursion();
        bool stackWrite  = recursionLockFunction.empty() && recursionLockInvocation < 0;
        if (!(traceAppend || stackWrite)) return *this;

        // Avoid per-char sends: accumulate
        std::ostringstream oss;
        oss << data;
        combined.append(oss.str());

        if (combined.size() >= 4096) flush();  // tune threshold as needed
        return *this;
    }

    LogWrapper& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (manip == static_cast<std::ostream&(*)(std::ostream&)>(std::endl)) {
            combined.push_back('\n');
            flush();
        }
        return *this;
    }

    static inline void flush() {
        if (combined.empty()) return;
        bool traceAppend = (getHitCount() <= MAX_DETAILED_HIT_COUNT) && !inDeepRecursion();
        bool stackWrite  = recursionLockFunction.empty() && recursionLockInvocation < 0;

        if (traceAppend && stackWrite) black_box_api::trace_and_stack(combined);
        else if (traceAppend)           black_box_api::trace_append(combined);
        else if (stackWrite)            black_box_api::stack_write(combined);

        combined.clear();
    }
};

inline std::string LogWrapper::combined;

// "log" is a global forwarding object.
// Note: In C++14 this "static" object will have internal linkage,
// meaning each translation unit including this header gets its own "log"
// wrapper, but all of them forward to the same Logger instance via get().
inline LogWrapper log;
inline PrintConfig cfg;

inline std::string getStack() {
    return stackLabels.back();
}

inline size_t getStackDepth() {
    return stackLabels.size();
}

static inline void write_crash_capsule() {
    black_box_api::memo_replace(getStack());
}

static void pushFrame(const std::string& function, int hitCount) {
    LogWrapper::flush();
    
    //*****
    //Mechanism to detect deep recursions
    if(recursionLockFunction.empty() || recursionLockInvocation < 0)
    {
        int recursionDepth = 0;
        for(const auto& it : frameStack)
        {
            if(it.function == function)
            {
                recursionDepth++;
            }
        }
        
        //Detected deep recursive calls. What is a good number here ?!?
        if(recursionDepth >= 30)
        {
            recursionLockFunction = function;
            recursionLockInvocation = hitCount;
            //get().lock(true);
            frameStack.push_back(StackFrame("DEEP_RECURSION", -1));
            
            if(!stackLabels.empty())
            {
                stackLabels.push_back(stackLabels.back() + "->DEEP_RECURSION:-1");
            }
            /*else
            {
                stackLabels.push_back("DEEP_RECURSION:-1");
            }*/
            
            //The purpose of the memo is to only keep the current call stack (in case of crash)
            //Ensure we have the memo printed with deep recursion only the first when detected.
            write_crash_capsule();
        }
    }
    
    if(!recursionLockFunction.empty() && recursionLockInvocation >= 0)
    {
        //We are in a deep recursion. Don't push this frame
        return;
    }
    //*****
    
    std::string hitCountStr = std::to_string(hitCount);
    frameStack.push_back(StackFrame(function, hitCount));
    if(!stackLabels.empty()) {
        stackLabels.push_back(stackLabels.back() + "->" + function + ":" + hitCountStr);
    }
    else {
        stackLabels.push_back(function + ":" + hitCountStr);
    }
    
    std::string filename = "stack/" + function + "." + hitCountStr + ".txt";

    black_box_api::stack_reopen(filename, true);
    
    //The purpose of the memo is to only keep the current call stack (in case of crash)
    write_crash_capsule();
}

static void popFrame(const std::string& function, int hitCount) {
    LogWrapper::flush();
    
    //Mechanism to detect deep recursions !?!?
    //*****
    if(
       (!recursionLockFunction.empty() && recursionLockInvocation >= 0) && //We are in a deep recursion
       (recursionLockFunction != function || recursionLockInvocation != hitCount) //Do we popup from a deep recursion ?
       )
    {
        //Don't pop frames under the deep recursion lock
        return;
    }
    
    recursionLockFunction.clear();
    recursionLockInvocation = -1;
    //*****
    
    if (!frameStack.empty())
    {
        // Make a safe local copy (or move) before we mutate the stack.
        // This avoids any chance of someone later using a dangling ref if code is rearranged.
        StackFrame sf = frameStack.back(); // or `StackFrame sf = std::move(frameStack.back());`

        if (sf.hasBreakpoints) {
            std::string functionBreakpoints;
            functionBreakpoints.reserve(sf.function.size() + 32);
            functionBreakpoints += sf.function;
            functionBreakpoints += '.';
            functionBreakpoints += std::to_string(sf.invocation);
            black_box_api::save_breakpoint(functionBreakpoints);
        }

        black_box_api::stack_remove();
        frameStack.pop_back();
        stackLabels.pop_back();
    }
    
    // 1) Pop logic and (optionally) reopen the top frame file
    if (!frameStack.empty()) {
        const auto& it = frameStack.back();
        std::string filename = "stack/" + it.function + "." + std::to_string(it.invocation) + ".txt";

        black_box_api::stack_reopen(filename, false);
    }

    // 2) Always refresh the crash capsule (even if the stack is now empty)
    write_crash_capsule();
}

static void printStack() {
    log << "Call stack: " << getStack() << std::endl;
}

static void hitBP(std::string atFunction,
                          int atLine,
                          const std::string& condition,
                          const std::string& expression)
{
    //Each section in a function needs unique name. Append ln number
    log << "<[PUSH (breakpoint ln " << atLine << "): " << getStack() << "]>" << std::endl;
    log << "Hit breakpoint at function: " << atFunction << " line: " << atLine << std::endl;
    if(!condition.empty())
    {
        log << " condition: (" << condition << ")" << std::endl;
    }
    
    if(!expression.empty())
    {
        log << " expression: (" << expression << ")" << std::endl;
    }
    
    if(!frameStack.empty())
    {
        //TODO: only for debug
        //log << "Tag frame hasBreakpoints=true callstack: " << getStack() << std::endl;
        
        auto& it = frameStack.back();
        it.hasBreakpoints = true;
    }
}

} // namespace trace

#include "data_printers.h"
#include "trace_printers.h"
