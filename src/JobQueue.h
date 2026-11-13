#pragma once

#include "Installer.h"
#include "Repo.h"
#include "Worker.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class JobState { Queued, Downloading, Verifying, Extracting, Done, Failed, Cancelled };

const char *jobStateName(JobState s);

struct Job {
    Mod mod;
    std::string gameName;
    std::string titleId;

    // Written by the worker, read by the UI (under JobQueue's mutex).
    JobState state = JobState::Queued;
    int64_t done = 0;
    int64_t total = -1;
    std::string error;

    std::atomic<bool> cancel{false};
};

// Runs install jobs one at a time on a background thread: download (with
// resume), optional SHA-256 check, then extract + swap via Installer.
class JobQueue {
public:
    explicit JobQueue(Installer &installer);
    ~JobQueue();

    void enqueue(const Mod &mod, const std::string &gameName, const std::string &titleId);
    void cancel(const std::shared_ptr<Job> &job);
    void clearFinished();

    // Copies of job state for drawing.
    struct View {
        std::shared_ptr<Job> job;
        JobState state;
        int64_t done, total;
        std::string error;
    };
    std::vector<View> snapshot() const;

    // Active (queued or running) job for a mod, if any.
    bool activeFor(const std::string &modId, View &out) const;
    int activeCount() const;

    // Incremented whenever a job finishes, so the UI knows to refresh.
    uint32_t generation() const { return generation_.load(); }

private:
    void run();
    void process(const std::shared_ptr<Job> &job);
    void setState(Job &job, JobState s);
    void setProgress(Job &job, int64_t done, int64_t total);

    Installer &installer_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Job>> pending_;
    std::vector<std::shared_ptr<Job>> all_;
    bool quit_ = false;
    std::atomic<uint32_t> generation_{0};
    std::unique_ptr<Worker> worker_;
};
