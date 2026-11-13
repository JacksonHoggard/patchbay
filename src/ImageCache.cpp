#include "ImageCache.h"
#include "Http.h"

#include <SDL2/SDL_image.h>
#include <vector>

static const size_t kMaxTextures = 48;
static const size_t kMaxPending = 16;

ImageCache::ImageCache(SDL_Renderer *renderer) : renderer_(renderer) {
    worker_.reset(new Worker([this] { run(); }));
}

ImageCache::~ImageCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    worker_.reset();
    for (auto &kv : entries_)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
}

SDL_Texture *ImageCache::get(const std::string &url) {
    if (url.empty()) return nullptr;
    Entry &e = entries_[url];
    e.lastUse = frame_;
    if (e.tex || e.failed) return e.tex;
    if (!e.requested) {
        e.requested = true;
        std::lock_guard<std::mutex> lock(mutex_);
        // Newest requests first: what is on screen now matters most.
        wanted_.push_front(url);
        while (wanted_.size() > kMaxPending) {
            entries_[wanted_.back()].requested = false;
            wanted_.pop_back();
        }
        cv_.notify_one();
    }
    return nullptr;
}

void ImageCache::update() {
    frame_++;
    std::deque<std::pair<std::string, std::string>> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(fetched_);
    }
    for (auto &d : done) {
        Entry &e = entries_[d.first];
        e.requested = false;
        if (e.tex) continue; // duplicate fetch of an image we already have
        if (d.second.empty()) {
            e.failed = true;
            continue;
        }
        SDL_RWops *rw = SDL_RWFromConstMem(d.second.data(), (int) d.second.size());
        e.tex = rw ? IMG_LoadTexture_RW(renderer_, rw, 1) : nullptr;
        if (!e.tex) e.failed = true;
    }

    // Evict least recently used textures beyond the cap.
    size_t live = 0;
    for (auto &kv : entries_)
        if (kv.second.tex) live++;
    while (live > kMaxTextures) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.tex && (oldest == entries_.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;
        if (oldest == entries_.end() || oldest->second.lastUse == frame_) break;
        SDL_DestroyTexture(oldest->second.tex);
        entries_.erase(oldest);
        live--;
    }
}

void ImageCache::run() {
    while (true) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !wanted_.empty(); });
            if (quit_) return;
            url = wanted_.front();
            wanted_.pop_front();
        }
        std::string body;
        auto res = http::get(url, body, 8 * 1024 * 1024);
        if (!res.ok) body.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        fetched_.emplace_back(url, std::move(body));
    }
}
