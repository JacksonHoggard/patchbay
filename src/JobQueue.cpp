#include "JobQueue.h"
#include "Fs.h"
#include "Http.h"
#include "Paths.h"
#include "Sha256.h"

const char *jobStateName(JobState s) {
    switch (s) {
    case JobState::Queued: return "Queued";
    case JobState::Downloading: return "Downloading";
    case JobState::Verifying: return "Verifying";
    case JobState::Extracting: return "Installing";
    case JobState::Building: return "Building";
    case JobState::Done: return "Done";
    case JobState::Failed: return "Failed";
    case JobState::Cancelled: return "Cancelled";
    }
    return "";
}

static bool isActive(JobState s) {
    return s == JobState::Queued || s == JobState::Downloading || s == JobState::Verifying ||
           s == JobState::Extracting || s == JobState::Building;
}

JobQueue::JobQueue(Installer &installer) : installer_(installer) {
    worker_.reset(new Worker([this] { run(); }));
}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        for (auto &j : all_) j->cancel = true;
    }
    cv_.notify_all();
    worker_.reset();
}

void JobQueue::enqueue(const Mod &mod, const std::string &gameName, const std::string &titleId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &j : all_)
        if (j->mod.id == mod.id && isActive(j->state)) return; // already queued

    auto job = std::make_shared<Job>();
    job->mod = mod;
    job->gameName = gameName;
    job->titleId = titleId;
    job->total = mod.fileSize;
    pending_.push_back(job);
    all_.push_back(job);
    cv_.notify_one();
}

void JobQueue::enqueueCombo(const std::string &comboId, const std::string &gameName,
                            const std::string &titleId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &j : all_)
        if (j->mod.id == comboId && isActive(j->state)) return; // already queued

    auto job = std::make_shared<Job>();
    job->isCombo = true;
    job->mod.id = comboId;
    job->mod.name = "Combo " + comboId;
    job->gameName = gameName;
    job->titleId = titleId;
    job->total = -1;
    pending_.push_back(job);
    all_.push_back(job);
    cv_.notify_one();
}

void JobQueue::cancel(const std::shared_ptr<Job> &job) {
    std::lock_guard<std::mutex> lock(mutex_);
    job->cancel = true;
    if (job->state == JobState::Queued) {
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if (*it == job) {
                pending_.erase(it);
                break;
            }
        job->state = JobState::Cancelled;
    }
}

void JobQueue::clearFinished() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<Job>> keep;
    for (auto &j : all_)
        if (isActive(j->state)) keep.push_back(j);
    all_.swap(keep);
}

std::vector<JobQueue::View> JobQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<View> out;
    for (auto &j : all_) out.push_back({j, j->state, j->done, j->total, j->error});
    return out;
}

bool JobQueue::activeFor(const std::string &modId, View &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &j : all_)
        if (j->mod.id == modId && isActive(j->state)) {
            out = {j, j->state, j->done, j->total, j->error};
            return true;
        }
    return false;
}

int JobQueue::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (auto &j : all_)
        if (isActive(j->state)) n++;
    return n;
}

void JobQueue::setState(Job &job, JobState s) {
    std::lock_guard<std::mutex> lock(mutex_);
    job.state = s;
    job.done = 0;
}

void JobQueue::setProgress(Job &job, int64_t done, int64_t total) {
    std::lock_guard<std::mutex> lock(mutex_);
    job.done = done;
    if (total > 0) job.total = total;
}

void JobQueue::run() {
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !pending_.empty(); });
            if (quit_) return;
            job = pending_.front();
            pending_.pop_front();
        }
        process(job);
        generation_++;
    }
}

void JobQueue::process(const std::shared_ptr<Job> &job) {
    Job &j = *job;
    const Mod &m = j.mod;
    auto fail = [&](const std::string &msg) {
        fs::log("job %s failed: %s", m.id.c_str(), msg.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        j.error = msg;
        j.state = j.cancel ? JobState::Cancelled : JobState::Failed;
    };

    // Combo: assemble from installed mods; nothing to download.
    if (j.isCombo) {
        setState(j, JobState::Building);
        std::string err;
        bool ok = installer_.buildCombo(m.id, [&](int64_t d, int64_t t) { setProgress(j, d, t); }, &j.cancel, err);
        if (!ok) return fail(err.empty() && j.cancel ? "Cancelled" : err);
        std::lock_guard<std::mutex> lock(mutex_);
        j.state = JobState::Done;
        return;
    }

    std::string zipPath = paths::downloadFile(m.id);

    // 1. Download (skipped if a complete file from an earlier attempt exists).
    setState(j, JobState::Downloading);
    int64_t have = fs::fileSize(zipPath);
    bool complete = m.fileSize > 0 && have == m.fileSize;
    if (!complete) {
        if (m.fileSize > 0 && have > m.fileSize) fs::removeAll(zipPath);
        auto res = http::download(
            m.download, zipPath, [&](int64_t d, int64_t t) { setProgress(j, d, t > 0 ? t : m.fileSize); },
            &j.cancel);
        if (!res.ok) {
            // Keep the partial file on cancel/network errors so it resumes.
            if (res.status >= 400 && res.status != 416) fs::removeAll(zipPath);
            return fail(res.error);
        }
    }
    int64_t size = fs::fileSize(zipPath);
    if (m.fileSize > 0 && size != m.fileSize)
        fs::log("job %s: size %lld differs from repo fileSize %lld", m.id.c_str(), (long long) size,
                (long long) m.fileSize);

    // 2. Verify.
    if (!m.sha256.empty()) {
        setState(j, JobState::Verifying);
        std::string got = sha256::ofFile(zipPath, [&](int64_t d, int64_t t) { setProgress(j, d, t); }, &j.cancel);
        if (j.cancel) return fail("Cancelled");
        if (got != m.sha256) {
            fs::removeAll(zipPath);
            return fail(got.empty() ? "Could not read download" : "SHA-256 mismatch: download rejected");
        }
    }

    // 3. Extract + swap in.
    setState(j, JobState::Extracting);
    std::string err;
    bool ok = installer_.install(m, j.gameName, j.titleId, zipPath, size,
                                 [&](int64_t d, int64_t t) { setProgress(j, d, t); }, &j.cancel, err);
    if (!ok) {
        // A corrupt archive will not get better on retry.
        if (!j.cancel) fs::removeAll(zipPath);
        return fail(err);
    }
    fs::removeAll(zipPath);

    std::lock_guard<std::mutex> lock(mutex_);
    j.state = JobState::Done;
}
