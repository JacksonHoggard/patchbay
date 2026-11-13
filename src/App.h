#pragma once

#include "Config.h"
#include "Gfx.h"
#include "ImageCache.h"
#include "Installer.h"
#include "JobQueue.h"
#include "Repo.h"
#include "Worker.h"

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

enum class Btn { Up, Down, Left, Right, A, B, X, Y, L, R, Plus, Minus, None };

enum class Screen { Loading, Games, Mods, Detail, Installed, Queue, Repos };

struct ListState {
    int sel = 0;
    int scroll = 0;
    void clamp(int count, int visible);
    void move(int delta, int count, int visible);
};

class App {
public:
    int run();

private:
    // Lifecycle
    bool init();
    void shutdown();
    void startLoad();
    void pollLoad();

    // Input
    void pumpEvents();
    void handle(Btn b);
    void handleModal(Btn b);

    // Screens
    void draw();
    void drawHeader();
    void drawFooter(const std::string &hints);
    void drawLoading();
    void drawGames();
    void drawMods();
    void drawDetail();
    void drawInstalled();
    void drawQueue();
    void drawRepos();
    void drawModal();
    void drawProgressBar(int x, int y, int w, int h, int64_t done, int64_t total, SDL_Color c);

    void handleGames(Btn b);
    void handleMods(Btn b);
    void handleDetail(Btn b);
    void handleInstalled(Btn b);
    void handleQueue(Btn b);
    void handleRepos(Btn b);

    // Actions
    void requestInstall(const Game &game, const Mod &mod);
    void requestUninstall(const std::string &modId, const std::string &name);
    void toggleEnabled(const std::string &modId);
    void openDetail(const std::string &modId);
    void requestExit();

    // Helpers
    const Game *currentGame() const;
    const Mod *currentMod() const;
    std::string statusLabel(const Mod &mod, SDL_Color &c) const;
    bool updateAvailable(const Mod &mod) const;
    static std::string formatSize(int64_t bytes);

    // Modal dialogs
    struct Modal {
        bool open = false;
        std::string title;
        std::vector<std::string> body;
        std::vector<std::string> options; // empty = message with OK
        int sel = 0;
        std::function<void(int)> onPick;  // index into options, -1 on cancel
    };
    void message(const std::string &title, const std::string &text);
    void choose(const std::string &title, const std::string &text, const std::vector<std::string> &options,
                std::function<void(int)> onPick);

    Gfx gfx_;
    std::unique_ptr<ImageCache> images_;
    Installer installer_;
    std::unique_ptr<JobQueue> jobs_;

    Config config_;
    Catalog catalog_;
    std::set<std::string> consoleTitles_;

    // Background repo loading
    std::unique_ptr<Worker> loader_;
    std::mutex loadMutex_;
    std::string loadStatus_;
    bool loadDone_ = false;
    Catalog loadResult_;
    Config loadConfig_;

    Screen screen_ = Screen::Loading;
    Screen detailReturn_ = Screen::Mods;
    ListState gamesList_, modsList_, installedList_, queueList_, reposList_;
    int detailScroll_ = 0;
    int detailMaxScroll_ = 0;
    std::string detailModId_;
    int gameIndex_ = 0;
    uint32_t seenJobGeneration_ = 0;

    Modal modal_;
    bool running_ = true;
    bool exiting_ = false;

    // Held-direction auto repeat
    Btn heldDir_ = Btn::None;
    uint32_t heldSince_ = 0, lastRepeat_ = 0;
    int axisY_ = 0, axisX_ = 0;
};
