#pragma once

#include "Worker.h"

#include <SDL2/SDL.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// Fetches remote images (thumbnails, game icons) in the background and turns
// them into textures on the render thread. Textures are evicted LRU.
class ImageCache {
public:
    explicit ImageCache(SDL_Renderer *renderer);
    ~ImageCache();

    // Returns the texture if ready, otherwise schedules a fetch and returns
    // nullptr. Must be called from the render thread.
    SDL_Texture *get(const std::string &url);

    // Call once per frame on the render thread.
    void update();

private:
    void run();

    struct Entry {
        SDL_Texture *tex = nullptr;
        bool failed = false;
        bool requested = false;
        uint64_t lastUse = 0;
    };

    SDL_Renderer *renderer_;
    std::map<std::string, Entry> entries_;
    uint64_t frame_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> wanted_;                 // render -> worker
    std::deque<std::pair<std::string, std::string>> fetched_; // worker -> render (url, bytes or "")
    bool quit_ = false;
    std::unique_ptr<Worker> worker_;
};
