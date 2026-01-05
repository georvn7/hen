#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class BlackBox {
public:
    //enum class Mode { Streaming, Buffered };

    // 'outDir' is where stdout.log, memo.txt, stack/* and breakpoints/* will be written.
    explicit BlackBox(uint16_t port, std::string outDir) noexcept;

    ~BlackBox();

    // Bind, listen, and spawn the worker thread. Returns false on immediate failure.
    bool start();

    // Ask the server to stop and flush everything (safe to call multiple times).
    void request_stop() noexcept;

    // Join the worker thread (no-op if not running).
    void join() noexcept;

    bool is_running() const noexcept { return running_.load(); }

private:
    void run();                 // worker thread entry
    void wakeup_accept();       // connect to our own socket to wake up accept()
    void close_client_fd();     // break a blocking recv()

    // Non-copyable
    BlackBox(const BlackBox&) = delete;
    BlackBox& operator=(const BlackBox&) = delete;

private:
    uint16_t    port_;
    std::string out_dir_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> listening_{false};

    std::thread thr_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
};
