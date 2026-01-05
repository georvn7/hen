#include "BlackBox.h"
#include "IncludeBoost.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "black_box_api.h" // message layout and codes

using namespace std;

// ======= server diag (one-line events) =======
namespace bbx_diag_s {
static inline std::string ts() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm; gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%FT%T") << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return oss.str();
}
static inline void log(const char* lvl, const char* ev, const std::string& kv) {
    std::cout << "ts=" << ts() << " lvl=" << lvl << " role=server event=" << ev
              << (kv.empty() ? "" : " ") << kv << '\n';
}
}
// ======= end server diag =======

static constexpr uint32_t MAX_STDOUT_LOG_BYTES = 25u * 1024u * 1024u; // 30 MB
static constexpr uint32_t HEAD_KEEP_SIZE      = 10u * 1024u * 1024u; // 10 MB
static constexpr uint32_t TAIL_KEEP_SIZE      =  5u * 1024u * 1024u; // 5  MB

struct MemStore {
    // --- stdout rolling buffer (all in memory) ---
    std::vector<char> stdout_buf;     // used only until first truncation
    std::vector<char> stdout_head;    // fixed after first truncation
    std::vector<char> stdout_tail;    // rolling ring: keep last TAIL_KEEP_SIZE bytes
    bool              stdout_truncated = false;
    uint64_t          stdout_trunc_bytes = 0; // total bytes dropped from the middle

    std::vector<char> trace_buf;

    // "live" memo + stack
    std::string memo_last;
    bool memo_seen = false;
    std::unordered_map<std::string, std::vector<char>> stack_files;

    // Breakpoint snapshots (purely in memory)
    // key = ABSOLUTE PATH under out_dir_/breakpoints/<bp>/frames/<...>
    std::unordered_map<std::string, std::vector<char>> bp_files;
    // key = sanitized <bp> name, value = memo text at snapshot time
    std::unordered_map<std::string, std::string>       bp_memos;

    std::string current_stack_path;
};

static inline std::string join_path(const std::string& base, const std::string& rel) {
    return (boost_fs::path(base) / rel).string();
}

static inline int xopen_append(const char* p) {
    return ::open(p, O_CREAT|O_APPEND|O_WRONLY|O_CLOEXEC, 0644);
}

static inline void ensure_dirs_for_file(const std::string& full_path) {
    auto dir = boost_fs::path(full_path).parent_path();
    if (!dir.empty()) boost_fs::create_directories(dir);
}

static int flush_data_to_disk(int fd, int strong) {
#if defined(__APPLE__)
    if (strong) { if (fcntl(fd, F_FULLFSYNC, 0) == 0) return 0; }
    return fsync(fd);
#else
    return fsync(fd);
#endif
}

// Precise reader with reason code
enum class ReadRC { Ok, Eof, Err };

static ReadRC read_exact_rc(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p); size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, c + got, n - got, 0);
        if (r == 0) return ReadRC::Eof;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return ReadRC::Err;
        }
        got += size_t(r);
    }
    return ReadRC::Ok;
}

// Legacy helper kept in case anything else calls it
static bool read_exact(int fd, void* p, size_t n, const std::atomic<bool>& /*stop*/) {
    return read_exact_rc(fd, p, n) == ReadRC::Ok;
}

BlackBox::BlackBox(uint16_t port, std::string outDir) noexcept
    : port_(port)
    , out_dir_(std::move(outDir)) {}

BlackBox::~BlackBox() {
    request_stop();
    join();
}

bool BlackBox::start() {
    if (running_.load()) return true;

    // Prepare output directory (does not chdir)
    boost_fs::create_directories(out_dir_);

    // Spawn worker thread
    try {
        thr_ = std::thread([this]{ run(); });
    } catch (...) {
        return false;
    }

    // Wait until listening socket is ready (cheap spin wait with timeout)
    for (int i = 0; i != 200; ++i) { // ~2s worst-case
        if (listening_.load()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // If we didn’t get to listening, ask it to stop and join
    request_stop();
    join();
    return false;
}

void BlackBox::request_stop() noexcept {
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) return;

    // Mark and wake accept() without cutting off unread bytes
    bbx_diag_s::log("INFO", "server_stop_requested", "phase=wake");
    wakeup_accept();

    // DO NOT forcibly shutdown client_fd_ here (keeps tail from being truncated)
    // close_client_fd();
}

void BlackBox::join() noexcept {
    if (thr_.joinable()) thr_.join();
}

void BlackBox::wakeup_accept() {
    // Connect to our own TCP port (127.0.0.1:port_) to wake accept()
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port_);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

    (void)::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)); // best-effort
    ::close(s);
}

void BlackBox::close_client_fd() {
    int fd = client_fd_;
    if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); /* ignore errors */ }
}

static void truncate_head_tail_to_path(const std::string& path, uint64_t &size_now) {
    struct stat st{}; if (::stat(path.c_str(), &st) != 0) { size_now = 0; return; }
    uint64_t fsz = (uint64_t)st.st_size;
    if (fsz <= MAX_STDOUT_LOG_BYTES) { size_now = fsz; return; }

    size_t head = (size_t)std::min<uint64_t>(fsz, HEAD_KEEP_SIZE);
    size_t tail = (size_t)std::min<uint64_t>(fsz, TAIL_KEEP_SIZE);
    uint64_t middle = fsz > (head + tail) ? (fsz - head - tail) : 0;

    std::vector<char> hbuf(head), tbuf(tail);
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            if (head) in.read(hbuf.data(), (std::streamsize)head);
            if (tail) {
                in.clear();
                in.seekg((std::streamoff)std::max<uint64_t>(0, fsz - tail), std::ios::beg);
                in.read(tbuf.data(), (std::streamsize)tail);
            }
        }
    }
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            if (head) out.write(hbuf.data(), (std::streamsize)hbuf.size());
            if (middle) {
                char marker[256];
                int n = std::snprintf(marker, sizeof(marker),
                                      "\n[TRUNCATED middle %llu bytes]\n",
                                      (unsigned long long)middle);
                if (n > 0) out.write(marker, n);
            }
            if (tail) out.write(tbuf.data(), (std::streamsize)tbuf.size());
        }
    }
    if (::stat(path.c_str(), &st) == 0) size_now = (uint64_t)st.st_size; else size_now = 0;
}

static void write_file_from_mem_to(const std::string& full_path, const std::vector<char>& buf) {
    ensure_dirs_for_file(full_path);
    int fd = ::open(full_path.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (!buf.empty()) (void)::write(fd, buf.data(), (int)buf.size());
    flush_data_to_disk(fd, 0);
    ::close(fd);
}

static void write_memo_atomic_to(const std::string& full_dir, const std::string& text) {
    const std::string tmp = (boost_fs::path(full_dir) / "memo.txt.tmp").string();
    const std::string fin = (boost_fs::path(full_dir) / "memo.txt").string();
    int tfd = ::open(tmp.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
    if (tfd >= 0) {
#if defined(F_NOCACHE)
        fcntl(tfd, F_NOCACHE, 1);
#endif
        (void)::write(tfd, text.data(), (int)text.size());
        ::fsync(tfd); ::close(tfd);
        ::rename(tmp.c_str(), fin.c_str());
        // fsync the directory
        int dfd = ::open(full_dir.c_str(), O_RDONLY);
        if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    }
}

static void flush_from_mem_to(const std::string& full_path, const std::vector<char>& src) {
    ensure_dirs_for_file(full_path);
    int fd = ::open(full_path.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
    if (fd < 0) return;
    const size_t n = src.size();
    if (n <= MAX_STDOUT_LOG_BYTES) {
        if (n) (void)::write(fd, src.data(), (int)n);
    } else {
        const size_t head = (size_t)std::min<uint64_t>(n, HEAD_KEEP_SIZE);
        const size_t tail = (size_t)std::min<uint64_t>(n, TAIL_KEEP_SIZE);
        const size_t mid  = n - head - tail;
        if (head) (void)::write(fd, src.data(), (int)head);
        char marker[256];
        const int m = std::snprintf(marker, sizeof(marker),
                                    "\n[TRUNCATED middle %llu bytes]\n", (unsigned long long)mid);
        if (m > 0) (void)::write(fd, marker, m);
        if (tail) (void)::write(fd, src.data() + (n - tail), (int)tail);
    }
    flush_data_to_disk(fd, 0);
    ::close(fd);
}

static std::string sanitize_component(const std::string& raw) {
    std::string s; s.reserve(raw.size());
    for (unsigned char ch : raw) {
        if ((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.') s.push_back((char)ch);
        else s.push_back('_');
    }
    if (s.empty()) s = "bp";
    if (s.size()>128) s.resize(128);
    return s;
}

static void stdout_append_and_truncate(MemStore& mem, std::string_view payload) {
    if (!mem.stdout_truncated) {
        // Still accumulating the full buffer
        mem.stdout_buf.insert(mem.stdout_buf.end(), payload.begin(), payload.end());
        const uint64_t total = (uint64_t)mem.stdout_buf.size();
        if (total > MAX_STDOUT_LOG_BYTES) {
            // Freeze HEAD and TAIL; start “truncated” mode
            const size_t head = std::min<uint64_t>(total, HEAD_KEEP_SIZE);
            const size_t tail = std::min<uint64_t>(total > head ? (total - head) : 0, (uint64_t)TAIL_KEEP_SIZE);
            const uint64_t mid = total - head - tail;

            mem.stdout_head.assign(mem.stdout_buf.begin(),
                                   mem.stdout_buf.begin() + (ptrdiff_t)head);
            mem.stdout_tail.assign(mem.stdout_buf.end() - (ptrdiff_t)tail,
                                   mem.stdout_buf.end());
            mem.stdout_buf.clear(); mem.stdout_buf.shrink_to_fit();

            mem.stdout_truncated   = true;
            mem.stdout_trunc_bytes = mid;
        }
        return;
    }

    // Already in truncated mode: grow tail up to TAIL_KEEP_SIZE
    mem.stdout_tail.insert(mem.stdout_tail.end(), payload.begin(), payload.end());

    if (mem.stdout_tail.size() > TAIL_KEEP_SIZE) {
        const size_t overflow = mem.stdout_tail.size() - TAIL_KEEP_SIZE;
        // Drop from FRONT of the tail; these bytes are now truly "truncated middle"
        mem.stdout_tail.erase(mem.stdout_tail.begin(),
                              mem.stdout_tail.begin() + (ptrdiff_t)overflow);
        mem.stdout_trunc_bytes += (uint64_t)overflow;
    }
    // NOTE: Do NOT add appended bytes when there is no overflow.
}

// On-demand materialization used by MSG_FLUSH_SYNC
static void flush_live_to_disk(const std::string& out_dir, const MemStore& mem) {
    // stdout.log snapshot (honor truncation mode)
    const std::string stdout_full = (boost_fs::path(out_dir) / "stdout.log").string();
    int fd = ::open(stdout_full.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
    if (fd >= 0) {
#if defined(F_NOCACHE)
        fcntl(fd, F_NOCACHE, 1);
#endif
        if (!mem.stdout_truncated) {
            if (!mem.stdout_buf.empty()) (void)::write(fd, mem.stdout_buf.data(), (int)mem.stdout_buf.size());
        } else {
            if (!mem.stdout_head.empty()) (void)::write(fd, mem.stdout_head.data(), (int)mem.stdout_head.size());
            char marker[128];
            const int m = std::snprintf(marker, sizeof(marker),
                                        "\n[TRUNCATED middle %llu bytes]\n",
                                        (unsigned long long)mem.stdout_trunc_bytes);
            if (m > 0) (void)::write(fd, marker, m);
            if (!mem.stdout_tail.empty()) (void)::write(fd, mem.stdout_tail.data(), (int)mem.stdout_tail.size());
        }
        (void)fsync(fd);
        ::close(fd);
    }

    // trace.txt snapshot
    const auto trace_full = join_path(out_dir, "trace.txt");
    flush_from_mem_to(trace_full, mem.trace_buf);
}

void BlackBox::run() {
    running_.store(true);

    // Counters / state for diagnostics
    uint64_t bytes_total = 0;
    uint64_t oversize_count = 0;
    uint64_t msgs_by_type[256] = {};
    bool trunc_announced = false;

    // Prepare / bind socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { running_.store(false); return; }

    const int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in sa{}; sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port_);

    if (::bind(listen_fd_, (sockaddr*)&sa, sizeof(sa)) != 0) {
        bbx_diag_s::log("ERROR", "bind_fail", "errno=" + std::to_string(errno));
        ::close(listen_fd_); running_.store(false); return;
    }
    if (::listen(listen_fd_, 1) != 0) {
        bbx_diag_s::log("ERROR", "listen_fail", "errno=" + std::to_string(errno));
        ::close(listen_fd_); running_.store(false); return;
    }
    bbx_diag_s::log("INFO", "listen_ok", "port=" + std::to_string(port_));

    int rcv = 4 * 1024 * 1024; (void)setsockopt(listen_fd_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);
    listening_.store(true);

    // accept single client (or until stop requested)
    client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd_ < 0) {
        bbx_diag_s::log("ERROR", "accept_fail", "errno=" + std::to_string(errno));
        ::close(listen_fd_); running_.store(false); return;
    }
    //::setsockopt(client_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    (void)setsockopt(client_fd_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    fcntl(client_fd_, F_SETFD, FD_CLOEXEC);
    bbx_diag_s::log("INFO", "accept_ok", "fd=" + std::to_string(client_fd_));

    MemStore mem;
    
    // Materialize stdout.log on-the-fly (like old file writer)
    const std::string stdout_path = (boost_fs::path(out_dir_) / "stdout.log").string();
    ensure_dirs_for_file(stdout_path);
    int stdout_fd = xopen_append(stdout_path.c_str());
    uint64_t stdout_size_now = 0;
    {
        struct stat st{};
        if (::stat(stdout_path.c_str(), &st) == 0) stdout_size_now = (uint64_t)st.st_size;
    }

    // Main loop
    while (!stop_.load()) {
        black_box_api::MsgHdr h{};
        auto rc_h = read_exact_rc(client_fd_, &h, sizeof(h));
        if (rc_h != ReadRC::Ok) {
            if (rc_h == ReadRC::Eof) bbx_diag_s::log("INFO", "peer_closed", "during=header");
            else                     bbx_diag_s::log("ERROR","recv_error", "during=header errno=" + std::to_string(errno));
            break;
        }

        std::string payload;
        if (h.len) {
            // defensive cap (8MiB like the standalone)
            static constexpr uint32_t MAX_MSG = 8u * 1024u * 1024u;
            if (h.len > MAX_MSG) {
                ++oversize_count;
                bbx_diag_s::log("ERROR","oversize_msg",
                    "type=" + std::to_string(h.type) +
                    " len=" + std::to_string(h.len) +
                    " max=" + std::to_string(MAX_MSG) +
                    " action=abort");
                break;
            }
            payload.resize(h.len);
            auto rc_p = read_exact_rc(client_fd_, (void*)payload.data(), (size_t)h.len);
            if (rc_p != ReadRC::Ok) {
                if (rc_p == ReadRC::Eof) bbx_diag_s::log("INFO", "peer_closed", "during=payload");
                else                     bbx_diag_s::log("ERROR","recv_error", "during=payload errno=" + std::to_string(errno));
                break;
            }
        }

        switch (h.type) {
        case black_box_api::MSG_STDOUT_APPEND: {
            const bool was = mem.stdout_truncated;
            stdout_append_and_truncate(mem, payload); // in-memory ring; keep this for stats

            // --- NEW: write-through to disk, then enforce 25 MB like the old layer ---
            if (stdout_fd >= 0 && !payload.empty()) {
                (void)::write(stdout_fd, payload.data(), (int)payload.size());
                stdout_size_now += (uint64_t)payload.size();

                if (stdout_size_now > MAX_STDOUT_LOG_BYTES) {
                    // Rewrite stdout.log to head + marker + tail (old behavior)
                    ::close(stdout_fd);
                    truncate_head_tail_to_path(stdout_path, stdout_size_now); // updates stdout_size_now
                    stdout_fd = xopen_append(stdout_path.c_str());
                }
            }

            if (!was && mem.stdout_truncated) {
                bbx_diag_s::log("WARN", "stdout_trunc_begin",
                                "dropped_mid_bytes=" + std::to_string((unsigned long long)mem.stdout_trunc_bytes));
            }
            break;
        }
        case black_box_api::MSG_TRACE_APPEND: {
            mem.trace_buf.insert(mem.trace_buf.end(), payload.begin(), payload.end());
            break;
        }
        case black_box_api::MSG_MEMO_REPLACE: {
            mem.memo_last.assign(payload.begin(), payload.end());
            mem.memo_seen = true;
            break;
        }
        case black_box_api::MSG_STACK_REOPEN: {
            bool trunc = !payload.empty() && (payload[0] != 0);
            std::string rel = payload.size() > 1 ? std::string(payload.data()+1, payload.size()-1) : std::string();
            std::string full = join_path(out_dir_, rel);
            mem.current_stack_path = full;
            auto& buf = mem.stack_files[full];
            if (trunc) buf.clear();
            break;
        }
        case black_box_api::MSG_STACK_REMOVE: {
            if (!mem.current_stack_path.empty()) mem.stack_files.erase(mem.current_stack_path);
            mem.current_stack_path.clear();
            break;
        }
        case black_box_api::MSG_STACK_WRITE: {
            if (!mem.current_stack_path.empty()) {
                auto& buf = mem.stack_files[mem.current_stack_path];
                buf.insert(buf.end(), payload.begin(), payload.end());
            }
            break;
        }
        case black_box_api::MSG_TRACE_AND_STACK: {
            mem.trace_buf.insert(mem.trace_buf.end(), payload.begin(), payload.end());
            
            auto& buf = mem.stack_files[mem.current_stack_path];
            buf.insert(buf.end(), payload.begin(), payload.end());
            break;
        }
        case black_box_api::MSG_SAVE_BREAKPOINT: {
            // payload: UTF-8 breakpoint name
            const std::string bp_name(payload.begin(), payload.end());
            const std::string safe = sanitize_component(bp_name);

            const boost_fs::path base_out   = boost_fs::path(out_dir_);
            const boost_fs::path stack_base = base_out / "stack";

            // Capture memo as it is now (might be empty if none seen yet)
            mem.bp_memos[safe] = mem.memo_seen ? mem.memo_last : std::string{};

            // Mirror every in-memory stack frame into the breakpoint snapshot map
            for (const auto& kv : mem.stack_files) {
                const std::string& abs_path_str = kv.first;  // key
                const auto&        buf          = kv.second; // value (whatever container/buffer you use)

                boost_fs::path abs(abs_path_str);

                // Compute relative path under stack/ to preserve directory layout
                boost::system::error_code ec_rel;
                boost_fs::path rel = boost_fs::relative(abs, stack_base, ec_rel);
                if (ec_rel || rel.empty() || rel.native().find("..") != std::string::npos) {
                    // fallback bucket for odd paths (shouldn't usually happen)
                    rel = boost_fs::path("misc") / abs.filename();
                }

                // Build the final destination path for when we later flush to disk
                boost_fs::path dst = base_out / "breakpoints" / safe / "frames" / rel;
                mem.bp_files[dst.string()] = buf;  // copy into snapshot
            }
            break;
        }
        case black_box_api::MSG_FLUSH_SYNC: {
            //flush_live_to_disk(out_dir_, mem);
            bbx_diag_s::log("INFO", "flush_sync", "ok=1");
            break;
        }
        case black_box_api::MSG_FINISH_STOP: {
            bbx_diag_s::log("INFO", "finish_stop_rx", "");
            stop_.store(true);
            break;
        }
        default:
            // Unknown types are harmless but useful to notice in dev
            bbx_diag_s::log("WARN", "unknown_type", "type=" + std::to_string(h.type));
            break;
        }

        // Counters
        bytes_total += h.len;
        if (h.type < 256) ++msgs_by_type[h.type];
    }

    // Final materialization at exit (single place)
    {
        // stdout.log from mem (honor truncation)
        const std::string stdout_full = (boost_fs::path(out_dir_) / "stdout.log").string();
        int fd = ::open(stdout_full.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
        if (fd >= 0) {
#if defined(F_NOCACHE)
            fcntl(fd, F_NOCACHE, 1);
#endif
            if (!mem.stdout_truncated) {
                if (!mem.stdout_buf.empty())
                    (void)::write(fd, mem.stdout_buf.data(), (int)mem.stdout_buf.size());
            } else {
                // HEAD
                if (!mem.stdout_head.empty())
                    (void)::write(fd, mem.stdout_head.data(), (int)mem.stdout_head.size());
                // MARKER
                char marker[128];
                const int m = std::snprintf(marker, sizeof(marker),
                                            "\n[TRUNCATED middle %llu bytes]\n",
                                            (unsigned long long)mem.stdout_trunc_bytes);
                if (m > 0) (void)::write(fd, marker, m);
                // TAIL
                if (!mem.stdout_tail.empty())
                    (void)::write(fd, mem.stdout_tail.data(), (int)mem.stdout_tail.size());
            }
            (void)fsync(fd);
            ::close(fd);
        }

        // memo.txt from mem
        if (mem.memo_seen) write_memo_atomic_to(out_dir_, mem.memo_last);
        else               write_memo_atomic_to(out_dir_, std::string{});

        // trace.txt from mem
        const auto trace_full = join_path(out_dir_, "trace.txt");
        flush_from_mem_to(trace_full, mem.trace_buf);

        // stack/ frames from mem
        for (const auto& kv : mem.stack_files) {
            const std::string& abs_dst = kv.first;
            const auto&        buf     = kv.second;

            ensure_dirs_for_file(abs_dst);
            write_file_from_mem_to(abs_dst, buf);
        }

        // breakpoints snapshots (frames + stack.txt) from mem.bp_files / mem.bp_memos
        for (const auto& kv : mem.bp_files) {
            const std::string& abs_dst = kv.first;
            const auto&        buf     = kv.second;
            ensure_dirs_for_file(abs_dst);
            write_file_from_mem_to(abs_dst, buf);
        }
        for (const auto& kv : mem.bp_memos) {
            const std::string& bp_name_safe = kv.first;
            const std::string& memo_text    = kv.second;
            boost_fs::path memo_dst = boost_fs::path(out_dir_) / "breakpoints" / bp_name_safe / "stack.txt";
            ensure_dirs_for_file(memo_dst.string());
            // atomic text writer
            {
                const std::string full = memo_dst.string();
                const boost_fs::path p(full);
                const std::string dir = p.parent_path().string();
                const std::string tmp = (p.parent_path() / (p.filename().string() + ".tmp")).string();
                int tfd = ::open(tmp.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
                if (tfd >= 0) {
        #if defined(F_NOCACHE)
                    fcntl(tfd, F_NOCACHE, 1);
        #endif
                    if (!memo_text.empty()) (void)::write(tfd, memo_text.data(), (int)memo_text.size());
                    ::fsync(tfd); ::close(tfd);
                    ::rename(tmp.c_str(), full.c_str());
                    int dfd = ::open(dir.c_str(), O_RDONLY);
                    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
                }
            }
        }
    }

    // One-line session summary
    {
        std::ostringstream kv;
        kv << "bytes_total=" << bytes_total
           << " oversize_count=" << oversize_count
           << " stdout_truncated=" << (mem.stdout_truncated ? 1 : 0)
           << " trunc_bytes=" << (unsigned long long)mem.stdout_trunc_bytes
           << " msgs_std=" << msgs_by_type[black_box_api::MSG_STDOUT_APPEND]
           << " msgs_trace=" << msgs_by_type[black_box_api::MSG_TRACE_APPEND]
           << " msgs_stackw=" << msgs_by_type[black_box_api::MSG_STACK_WRITE];
        bbx_diag_s::log("INFO", "session_summary", kv.str());
    }

    if (client_fd_ >= 0) ::close(client_fd_);
    if (listen_fd_ >= 0) ::close(listen_fd_);

    running_.store(false);
}
