#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <map>
#include <string>
#include <vector>

enum class FontSize { Small = 0, Normal, Large, Title, Count };

namespace color {
constexpr SDL_Color Bg{18, 20, 26, 255};
constexpr SDL_Color Panel{30, 33, 42, 255};
constexpr SDL_Color PanelHi{48, 54, 70, 255};
constexpr SDL_Color Accent{255, 167, 38, 255};   // amber
constexpr SDL_Color Accent2{77, 182, 172, 255};  // teal
constexpr SDL_Color Text{235, 236, 240, 255};
constexpr SDL_Color Dim{150, 155, 168, 255};
constexpr SDL_Color Good{102, 187, 106, 255};
constexpr SDL_Color Bad{239, 83, 80, 255};
constexpr SDL_Color Warn{255, 213, 79, 255};
} // namespace color

class Gfx {
public:
    static constexpr int W = 1280;
    static constexpr int H = 720;

    bool init();
    void shutdown();

    SDL_Renderer *renderer() const { return renderer_; }

    void begin();
    void end();

    void fill(int x, int y, int w, int h, SDL_Color c);
    void outline(int x, int y, int w, int h, SDL_Color c, int thickness = 2);
    void image(SDL_Texture *tex, int x, int y, int w, int h, bool keepAspect = true);

    // Draws single-line text. If maxW > 0 the text is truncated with "...".
    // align: 0 left, 1 center, 2 right (relative to x / x+maxW).
    int text(const std::string &s, int x, int y, FontSize f, SDL_Color c, int maxW = 0, int align = 0);
    int textWidth(const std::string &s, FontSize f);
    int lineHeight(FontSize f);
    std::vector<std::string> wrap(const std::string &s, FontSize f, int maxW);

private:
    struct Cached {
        SDL_Texture *tex;
        int w, h;
        uint64_t lastUse;
    };
    Cached *render(const std::string &s, FontSize f, SDL_Color c);

    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    TTF_Font *fonts_[(int) FontSize::Count] = {};
    std::map<std::string, Cached> cache_;
    uint64_t frame_ = 0;
};
