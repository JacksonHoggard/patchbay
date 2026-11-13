#include "Gfx.h"
#include "Fs.h"

#include <SDL2/SDL_image.h>
#include <algorithm>
#include <coreinit/memory.h>

static const int kPointSizes[(int) FontSize::Count] = {20, 26, 32, 44};

bool Gfx::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        fs::log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    window_ = SDL_CreateWindow("PatchBay", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, 0);
    if (!window_) {
        fs::log("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        fs::log("SDL_CreateRenderer: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    if (TTF_Init() != 0) {
        fs::log("TTF_Init: %s", TTF_GetError());
        return false;
    }
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);

    // Use the console's built-in system font instead of shipping one.
    void *fontData = nullptr;
    uint32_t fontSize = 0;
    if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &fontData, &fontSize) || !fontData) {
        fs::log("OSGetSharedData(font) failed");
        return false;
    }
    for (int i = 0; i < (int) FontSize::Count; ++i) {
        SDL_RWops *rw = SDL_RWFromConstMem(fontData, (int) fontSize);
        fonts_[i] = TTF_OpenFontRW(rw, 1, kPointSizes[i]);
        if (!fonts_[i]) {
            fs::log("TTF_OpenFontRW: %s", TTF_GetError());
            return false;
        }
    }
    return true;
}

void Gfx::shutdown() {
    for (auto &kv : cache_) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
    for (auto &f : fonts_)
        if (f) TTF_CloseFont(f);
    TTF_Quit();
    IMG_Quit();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Gfx::begin() {
    frame_++;
    SDL_SetRenderDrawColor(renderer_, color::Bg.r, color::Bg.g, color::Bg.b, 255);
    SDL_RenderClear(renderer_);
}

void Gfx::end() {
    SDL_RenderPresent(renderer_);
    // Drop text textures not drawn for ~2 seconds.
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (frame_ - it->second.lastUse > 120) {
            SDL_DestroyTexture(it->second.tex);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void Gfx::fill(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(renderer_, &r);
}

void Gfx::outline(int x, int y, int w, int h, SDL_Color c, int t) {
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c);
    fill(x + w - t, y, t, h, c);
}

void Gfx::image(SDL_Texture *tex, int x, int y, int w, int h, bool keepAspect) {
    if (!tex) return;
    SDL_Rect dst{x, y, w, h};
    if (keepAspect) {
        int tw, th;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        if (tw > 0 && th > 0) {
            double s = std::min((double) w / tw, (double) h / th);
            dst.w = (int) (tw * s);
            dst.h = (int) (th * s);
            dst.x = x + (w - dst.w) / 2;
            dst.y = y + (h - dst.h) / 2;
        }
    }
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
}

Gfx::Cached *Gfx::render(const std::string &s, FontSize f, SDL_Color c) {
    if (s.empty()) return nullptr;
    char key[32];
    SDL_snprintf(key, sizeof(key), "%d:%02x%02x%02x%02x:", (int) f, c.r, c.g, c.b, c.a);
    std::string k = key + s;
    auto it = cache_.find(k);
    if (it != cache_.end()) {
        it->second.lastUse = frame_;
        return &it->second;
    }
    SDL_Surface *surf = TTF_RenderUTF8_Blended(fonts_[(int) f], s.c_str(), c);
    if (!surf) return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surf);
    Cached cd{tex, surf->w, surf->h, frame_};
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    return &(cache_[k] = cd);
}

int Gfx::textWidth(const std::string &s, FontSize f) {
    int w = 0, h = 0;
    if (!s.empty()) TTF_SizeUTF8(fonts_[(int) f], s.c_str(), &w, &h);
    return w;
}

int Gfx::lineHeight(FontSize f) {
    return TTF_FontLineSkip(fonts_[(int) f]);
}

// Byte length of the UTF-8 sequence starting at s[i].
static size_t u8len(const std::string &s, size_t i) {
    unsigned char c = (unsigned char) s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    return i + n > s.size() ? s.size() - i : n;
}

int Gfx::text(const std::string &s, int x, int y, FontSize f, SDL_Color c, int maxW, int align) {
    std::string str = s;
    if (maxW > 0 && textWidth(str, f) > maxW) {
        // Trim by whole UTF-8 characters until it fits with an ellipsis.
        std::vector<size_t> starts;
        for (size_t i = 0; i < str.size(); i += u8len(str, i)) starts.push_back(i);
        size_t lo = 0, hi = starts.size();
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            if (textWidth(str.substr(0, starts[mid - 1] + u8len(str, starts[mid - 1])) + "...", f) <= maxW)
                lo = mid;
            else
                hi = mid - 1;
        }
        str = (lo == 0 ? std::string() : str.substr(0, starts[lo - 1] + u8len(str, starts[lo - 1]))) + "...";
    }
    Cached *cd = render(str, f, c);
    if (!cd) return 0;
    int dx = x;
    if (maxW > 0 && align == 1) dx = x + (maxW - cd->w) / 2;
    if (maxW > 0 && align == 2) dx = x + maxW - cd->w;
    SDL_Rect dst{dx, y, cd->w, cd->h};
    SDL_RenderCopy(renderer_, cd->tex, nullptr, &dst);
    return cd->w;
}

std::vector<std::string> Gfx::wrap(const std::string &s, FontSize f, int maxW) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!para.empty() && para.back() == '\r') para.pop_back();

        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string cand = line.empty() ? word : line + " " + word;
            if (textWidth(cand, f) <= maxW) {
                line = cand;
            } else {
                if (!line.empty()) lines.push_back(line);
                // Hard-break words that are wider than the box (URLs).
                line.clear();
                std::string w;
                for (size_t k = 0; k < word.size(); k += u8len(word, k)) {
                    std::string ch = word.substr(k, u8len(word, k));
                    if (!w.empty() && textWidth(w + ch, f) > maxW) {
                        lines.push_back(w);
                        w.clear();
                    }
                    w += ch;
                }
                line = w;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}
