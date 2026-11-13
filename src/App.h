#pragma once

#include "Config.h"
#include "GameBanana.h"
#include "Gfx.h"
#include "ImageCache.h"
#include "Installer.h"
#include "JobQueue.h"
#include "Repo.h"
#include "Worker.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

enum class Btn { Up, Down, Left, Right, A, B, X, Y, L, R, Plus, Minus, None };

enum class Screen { Loading, Games, Mods, Detail, Installed, Combos, ComboEdit, Queue, Repos };

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
    void pollGb();

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
    void drawCombos();
    void drawComboEdit();
    void drawModal();
    void drawProgressBar(int x, int y, int w, int h, int64_t done, int64_t total, SDL_Color c);

    void handleGames(Btn b);
    void handleMods(Btn b);
    void handleDetail(Btn b);
    void handleInstalled(Btn b);
    void handleQueue(Btn b);
    void handleRepos(Btn b);
    void handleCombos(Btn b);
    void handleComboEdit(Btn b);

    // Actions
    void requestInstall(const Game &game, const Mod &mod);
    void requestUninstall(const std::string &modId, const std::string &name);
    void toggleEnabled(const std::string &modId);
    void openDetail(const std::string &modId);
    void installGbMod(const std::string &modId);
    void requestExit();
    void newCombo();
    void openComboEditor(const Combo &combo, bool isNew);
    void saveAndBuildCombo();

    // Combo editor rows: members (priority order) first, then other installed
    // mods for the same title.
    struct ComboRow {
        std::string modId;
        std::string name;
        int memberIndex; // -1 = not a member
    };
    std::vector<ComboRow> comboEditRows() const;
    void selectComboRow(const std::string &modId);
    void updateComboPlan();

    // Helpers
    const Game *currentGame() const;
    const Mod *currentMod() const;
    // Catalog mod, or a GameBanana mod seen this session. `game` gets the
    // catalog game it belongs to (nullptr if unknown).
    const Mod *findAnyMod(const std::string &id, const Game **game = nullptr) const;
    int gameIndexOf(const Game *g) const;
    static bool isGbModId(const std::string &id);
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

    // ---- GameBanana
    // List for one game + sort. Everything except the *Result_ hand-off is
    // touched only on the UI thread. gbGen_ changes whenever the list is reset;
    // a page fetched for an older generation is dropped.
    void gbOpenList(int gbGameId);
    void gbResetList();
    bool gbLoading() const;
    void gbMaybeLoadMore();
    void gbStartFetch();
    void gbRequestDetails(int64_t gbModId);
    void gbStartDetails();

    bool gbMode_ = false;              // Mods screen shows GameBanana
    gb::Sort gbSort_ = gb::Sort::MostDownloaded;
    int gbListGameId_ = 0;             // gbGameId the list belongs to
    std::vector<std::string> gbList_;  // mod ids in display order
    std::set<std::string> gbListIds_;
    int gbPage_ = 0;                   // last page appended (1-based)
    int gbTotal_ = 0;
    bool gbComplete_ = false;
    bool gbWant_ = false;              // fetch gbPage_ + 1 when the worker is free
    std::string gbError_;
    uint32_t gbGen_ = 0;
    bool gbBusy_ = false;
    uint32_t gbInFlightGen_ = 0;
    std::map<std::string, Mod> gbMods_;    // every GameBanana mod seen, by id
    std::map<std::string, int> gbModGame_; // mod id -> gbGameId
    bool gbNoticeShown_ = false;

    struct GbPageResult {
        uint32_t gen = 0;
        int gameId = 0;
        int page = 0;
        gb::Page data;
    };
    std::unique_ptr<Worker> gbWorker_;
    std::mutex gbMutex_;
    bool gbResultReady_ = false;
    GbPageResult gbResult_;

    // Details, cached per GameBanana mod id. Results are keyed by id, so one
    // that arrives after the user moved on still fills the cache.
    std::map<int64_t, gb::Details> gbDetails_;
    std::unique_ptr<Worker> gbDetailWorker_;
    std::mutex gbDetailMutex_;
    bool gbDetailReady_ = false;
    int64_t gbDetailResultId_ = 0;
    gb::Details gbDetailResult_;
    bool gbDetailBusy_ = false;
    int64_t gbDetailInFlight_ = 0;
    int64_t gbDetailWant_ = 0;

    // Combo editor
    Combo comboEdit_;
    bool comboIsNew_ = false;
    bool comboDirty_ = false;
    bool comboPlanNeeded_ = false;

    // Background merge preview (planCombo walks the SD card). planGen_ is
    // written on the UI thread under planMutex_; results from an older
    // generation are dropped.
    std::unique_ptr<Worker> planWorker_;
    std::atomic<bool> planCancel_{false};
    std::mutex planMutex_;
    bool planRunning_ = false;
    uint32_t planGen_ = 0;
    uint32_t planResultGen_ = 0;
    ComboPlan plan_; // copies cleared; see planFiles_
    int planFiles_ = 0;

    Screen screen_ = Screen::Loading;
    Screen detailReturn_ = Screen::Mods;
    ListState gamesList_, modsList_, installedList_, queueList_, reposList_, combosList_, comboEditList_;
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
