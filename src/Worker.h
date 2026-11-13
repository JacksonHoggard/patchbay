#pragma once

#include <coreinit/thread.h>
#include <functional>
#include <memory>

// A background thread with an explicit, large stack. TLS handshakes in
// mbedTLS need far more stack than the default thread size, so all network
// work runs on one of these.
class Worker {
public:
    explicit Worker(std::function<void()> fn, uint32_t stackSize = 512 * 1024);
    ~Worker(); // joins
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    void join();
    bool ok() const { return thread_ != nullptr; }

private:
    static int entry(int argc, void *argv);

    std::function<void()> fn_;
    OSThread *thread_ = nullptr;
    void *stack_ = nullptr;
    bool joined_ = false;
};
