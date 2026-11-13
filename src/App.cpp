#include "App.h"
#include "Fs.h"
#include "Http.h"
#include "Paths.h"
#include "Platform.h"
#include "Titles.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace {

constexpr int kHeaderH = 72;
constexpr int kFooterH = 52;
constexpr int kContentY = kHeaderH + 12;
constexpr int kContentH = Gfx::H - kHeaderH - kFooterH - 24;
constexpr int kMargin = 40;
constexpr int kRowH = 64;

const Screen kTabs[] = {Screen::Games, Screen::Installed, Screen::Combos, Screen::Queue, Screen::Repos};
constexpr int kTabCount = (int) (sizeof(kTabs) / sizeof(kTabs[0]));

std::string join(const std::vector<std::string> &v, const std::string &sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

} // namespace

// ---------------------------------------------------------------- ListState

void ListState::clamp(int count, int visible) {
    if (count <= 0) {
        sel = scroll = 0;
        return;
    }
    sel = std::max(0, std::min(sel, count - 1));
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;
    scroll = std::max(0, std::min(scroll, std::max(0, count - visible)));
}

void ListState::move(int delta, int count, int visible) {
    if (count <= 0) return;
    int next = sel + delta;
    // Wrap only on single steps; page jumps clamp.
    if (delta == 1 || delta == -1) next = (next + count) % count;
    sel = next;
    clamp(count, visible);
}

// ---------------------------------------------------------------- lifecycle

int App::run() {
    if (!init()) {
        shutdown();
        return 1;
    }
    while (running_) {
        pumpEvents();
        pollLoad();
        images_->update();
        gfx_.begin();
        draw();
        gfx_.end();
    }
    shutdown();
    return 0;
}

bool App::init() {
    fs::mkdirs(paths::kAppDir);
    fs::mkdirs(paths::kTmpDir);
    fs::mkdirs(paths::kSdCafiine);
    fs::log("PatchBay %s starting", PATCHBAY_VERSION);

    platform::init();
    fs::log("launched from %s", platform::runningFromHbl() ? "Homebrew Launcher" : "Wii U Menu");
    if (!gfx_.init()) return false;
    if (!http::globalInit()) fs::log("curl_global_init failed");

    images_.reset(new ImageCache(gfx_.renderer()));
    installer_.load();
    installer_.recover();
    jobs_.reset(new JobQueue(installer_));
    consoleTitles_ = titles::installed();
    fs::log("%u titles installed on console", (unsigned) consoleTitles_.size());

    startLoad();
    return true;
}

void App::shutdown() {
    // Order matters: stop threads that use curl before tearing it down.
    http::abortAll();
    loader_.reset();
    planCancel_ = true;
    planWorker_.reset();
    jobs_.reset();
    images_.reset();
    http::globalCleanup();
    gfx_.shutdown();
    platform::shutdown();
}

static void loadStatusCb(const std::string &s, void *ud) {
    auto *pair = static_cast<std::pair<std::mutex *, std::string *> *>(ud);
    std::lock_guard<std::mutex> lock(*pair->first);
    *pair->second = s;
}

void App::startLoad() {
    if (loader_) loader_.reset(); // joins a previous load
    screen_ = Screen::Loading;
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        loadDone_ = false;
        loadStatus_ = "Reading config...";
    }
    loader_.reset(new Worker([this] {
        {
            std::lock_guard<std::mutex> lock(loadMutex_);
            loadStatus_ = "Connecting to network...";
        }
        platform::connectNetwork();
        Config cfg = Config::load();
        std::pair<std::mutex *, std::string *> ud{&loadMutex_, &loadStatus_};
        Catalog cat = repo::loadAll(cfg.repos, loadStatusCb, &ud);
        std::lock_guard<std::mutex> lock(loadMutex_);
        loadConfig_ = std::move(cfg);
        loadResult_ = std::move(cat);
        loadDone_ = true;
    }));
}

void App::pollLoad() {
    if (screen_ != Screen::Loading) return;
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        if (!loadDone_) return;
        config_ = std::move(loadConfig_);
        catalog_ = std::move(loadResult_);
        loadDone_ = false;
    }
    loader_.reset();
    installer_.adoptExisting(catalog_);
    gamesList_ = ListState();
    screen_ = (config_.repos.empty() || catalog_.games.empty()) ? Screen::Repos : Screen::Games;
}

// ---------------------------------------------------------------- input

static Btn fromController(Uint8 b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return Btn::Up;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Btn::Down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Btn::Left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Btn::Right;
    case SDL_CONTROLLER_BUTTON_A: return Btn::A;
    case SDL_CONTROLLER_BUTTON_B: return Btn::B;
    case SDL_CONTROLLER_BUTTON_X: return Btn::X;
    case SDL_CONTROLLER_BUTTON_Y: return Btn::Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Btn::L;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Btn::R;
    case SDL_CONTROLLER_BUTTON_START: return Btn::Plus;
    case SDL_CONTROLLER_BUTTON_BACK: return Btn::Minus;
    default: return Btn::None;
    }
}

static Btn fromKey(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return Btn::Up;
    case SDLK_DOWN: return Btn::Down;
    case SDLK_LEFT: return Btn::Left;
    case SDLK_RIGHT: return Btn::Right;
    case SDLK_RETURN: return Btn::A;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE: return Btn::B;
    case SDLK_x: return Btn::X;
    case SDLK_y: return Btn::Y;
    case SDLK_q: return Btn::L;
    case SDLK_e: return Btn::R;
    case SDLK_TAB: return Btn::Minus;
    default: return Btn::None;
    }
}

static bool isDir(Btn b) {
    return b == Btn::Up || b == Btn::Down || b == Btn::Left || b == Btn::Right;
}

void App::pumpEvents() {
    SDL_Event e;
    Btn pressed = Btn::None;
    while (SDL_PollEvent(&e)) {
        Btn b = Btn::None;
        switch (e.type) {
        case SDL_QUIT:
            running_ = false;
            return;
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            b = fromController(e.cbutton.button);
            break;
        case SDL_CONTROLLERBUTTONUP:
            if (fromController(e.cbutton.button) == heldDir_) heldDir_ = Btn::None;
            break;
        case SDL_KEYDOWN:
            if (!e.key.repeat) b = fromKey(e.key.keysym.sym);
            break;
        case SDL_KEYUP:
            if (fromKey(e.key.keysym.sym) == heldDir_) heldDir_ = Btn::None;
            break;
        case SDL_CONTROLLERAXISMOTION: {
            const int on = 20000, off = 12000;
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                int v = e.caxis.value;
                int dir = v > on ? 1 : v < -on ? -1 : (std::abs(v) < off ? 0 : axisY_);
                if (dir != axisY_) {
                    axisY_ = dir;
                    if (dir) b = dir > 0 ? Btn::Down : Btn::Up;
                    else if (heldDir_ == Btn::Up || heldDir_ == Btn::Down) heldDir_ = Btn::None;
                }
            } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                int v = e.caxis.value;
                int dir = v > on ? 1 : v < -on ? -1 : (std::abs(v) < off ? 0 : axisX_);
                if (dir != axisX_) {
                    axisX_ = dir;
                    if (dir) b = dir > 0 ? Btn::Right : Btn::Left;
                    else if (heldDir_ == Btn::Left || heldDir_ == Btn::Right) heldDir_ = Btn::None;
                }
            }
            break;
        }
        default: break;
        }
        if (b != Btn::None) {
            if (isDir(b)) {
                heldDir_ = b;
                heldSince_ = lastRepeat_ = SDL_GetTicks();
            }
            pressed = b;
            handle(b);
            if (!running_) return;
        }
    }
    // Auto-repeat while a direction is held.
    if (heldDir_ != Btn::None && pressed == Btn::None) {
        uint32_t now = SDL_GetTicks();
        if (now - heldSince_ > 350 && now - lastRepeat_ > 60) {
            lastRepeat_ = now;
            handle(heldDir_);
        }
    }
}

void App::handle(Btn b) {
    if (modal_.open) {
        handleModal(b);
        return;
    }
    if (exiting_) return;

    // Top-level tab switching.
    bool topLevel = screen_ == Screen::Games || screen_ == Screen::Installed || screen_ == Screen::Combos ||
                    screen_ == Screen::Queue || screen_ == Screen::Repos;
    if (topLevel && (b == Btn::L || b == Btn::R)) {
        int idx = 0;
        for (int i = 0; i < kTabCount; ++i)
            if (kTabs[i] == screen_) idx = i;
        idx = (idx + (b == Btn::R ? 1 : kTabCount - 1)) % kTabCount;
        screen_ = kTabs[idx];
        return;
    }
    if (b == Btn::Plus) {
        requestExit();
        return;
    }

    switch (screen_) {
    case Screen::Loading: break;
    case Screen::Games: handleGames(b); break;
    case Screen::Mods: handleMods(b); break;
    case Screen::Detail: handleDetail(b); break;
    case Screen::Installed: handleInstalled(b); break;
    case Screen::Combos: handleCombos(b); break;
    case Screen::ComboEdit: handleComboEdit(b); break;
    case Screen::Queue: handleQueue(b); break;
    case Screen::Repos: handleRepos(b); break;
    }
}

void App::handleModal(Btn b) {
    Modal &m = modal_;
    int n = m.options.empty() ? 1 : (int) m.options.size();
    if (b == Btn::Up || b == Btn::Left) m.sel = (m.sel + n - 1) % n;
    else if (b == Btn::Down || b == Btn::Right) m.sel = (m.sel + 1) % n;
    else if (b == Btn::A || b == Btn::B) {
        int pick = b == Btn::A ? m.sel : -1;
        auto cb = std::move(m.onPick);
        m = Modal();
        if (cb) cb(pick);
    }
}

void App::message(const std::string &title, const std::string &text) {
    modal_ = Modal();
    modal_.open = true;
    modal_.title = title;
    modal_.body = gfx_.wrap(text, FontSize::Normal, 640);
}

void App::choose(const std::string &title, const std::string &text, const std::vector<std::string> &options,
                 std::function<void(int)> onPick) {
    modal_ = Modal();
    modal_.open = true;
    modal_.title = title;
    modal_.body = text.empty() ? std::vector<std::string>() : gfx_.wrap(text, FontSize::Normal, 640);
    modal_.options = options;
    modal_.onPick = std::move(onPick);
}

// ---------------------------------------------------------------- helpers

const Game *App::currentGame() const {
    if (gameIndex_ < 0 || gameIndex_ >= (int) catalog_.games.size()) return nullptr;
    return &catalog_.games[gameIndex_];
}

const Mod *App::currentMod() const {
    return catalog_.findMod(detailModId_);
}

bool App::updateAvailable(const Mod &mod) const {
    InstalledMod im;
    if (!installer_.get(mod.id, im) || im.version.empty()) return false;
    return Installer::compareVersions(mod.version, im.version) > 0;
}

std::string App::statusLabel(const Mod &mod, SDL_Color &c) const {
    JobQueue::View v;
    if (jobs_->activeFor(mod.id, v)) {
        c = color::Accent2;
        if (v.state == JobState::Queued) return "Queued";
        int pct = v.total > 0 ? (int) (v.done * 100 / v.total) : 0;
        return std::string(jobStateName(v.state)) + " " + std::to_string(pct) + "%";
    }
    InstalledMod im;
    if (!installer_.get(mod.id, im)) return "";
    if (updateAvailable(mod)) {
        c = color::Warn;
        return "Update";
    }
    if (!im.enabled) {
        c = color::Dim;
        return "Disabled";
    }
    c = color::Good;
    return "Installed";
}

std::string App::formatSize(int64_t bytes) {
    if (bytes < 0) return "?";
    char buf[32];
    if (bytes >= 1024LL * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%lld B", (long long) bytes);
    return buf;
}

// ---------------------------------------------------------------- actions

void App::requestInstall(const Game &game, const Mod &mod) {
    std::string gameName = game.name;
    Mod modCopy = mod;
    auto enqueue = [this, gameName, modCopy](const std::string &tid) {
        jobs_->enqueue(modCopy, gameName, tid);
        auto others = installer_.enabledForTitle(tid, modCopy.id);
        if (!others.empty()) {
            std::vector<std::string> names;
            for (auto &o : others) names.push_back(o.name);
            message("Queued",
                    modCopy.name + " was added to the download queue.\n\nOther enabled mods for this game: " +
                        join(names, ", ") +
                        ".\nSDCafiine will ask which one to load when the game starts. Disable mods you do "
                        "not want from the Installed tab.");
        }
    };

    // Updating keeps the region it was installed for.
    InstalledMod im;
    if (installer_.get(mod.id, im)) {
        enqueue(im.titleId);
        return;
    }

    std::vector<std::string> present;
    for (auto &t : game.titleIds)
        if (consoleTitles_.count(t)) present.push_back(t);

    if (present.size() == 1) {
        enqueue(present[0]);
        return;
    }

    std::vector<std::string> choices = present.empty() ? game.titleIds : present;
    std::vector<std::string> labels;
    for (auto &t : choices) labels.push_back(t + (consoleTitles_.count(t) ? "  (installed)" : ""));
    std::string text = present.empty()
                           ? game.name + " was not found on this console. Pick the title ID (region) to install for:"
                           : "More than one region of " + game.name + " is installed. Install for:";
    choose("Select region", text, labels, [enqueue, choices](int i) {
        if (i >= 0) enqueue(choices[(size_t) i]);
    });
}

void App::requestUninstall(const std::string &modId, const std::string &name) {
    choose("Uninstall", "Delete " + name + " from the SD card?", {"Cancel", "Uninstall"}, [this, modId](int i) {
        if (i != 1) return;
        std::string err;
        if (!installer_.uninstall(modId, err)) message("Uninstall failed", err);
    });
}

void App::toggleEnabled(const std::string &modId) {
    InstalledMod im;
    if (!installer_.get(modId, im)) return;
    std::string err;
    if (!installer_.setEnabled(modId, !im.enabled, err)) message("Error", err);
}

void App::openDetail(const std::string &modId) {
    detailModId_ = modId;
    detailScroll_ = 0;
    detailReturn_ = screen_;
    const Game *g = nullptr;
    if (catalog_.findMod(modId, &g) && g) {
        for (size_t i = 0; i < catalog_.games.size(); ++i)
            if (&catalog_.games[i] == g) gameIndex_ = (int) i;
        screen_ = Screen::Detail;
    }
}

void App::requestExit() {
    auto doExit = [this] {
        exiting_ = true;
        // Under ProcUI the app must ask the system to leave; SDL then
        // delivers SDL_QUIT once the switch happens.
        platform::requestExit();
    };
    if (jobs_->activeCount() > 0) {
        choose("Exit", "Downloads are still running. Unfinished downloads resume next time.", {"Stay", "Exit"},
               [doExit](int i) {
                   if (i == 1) doExit();
               });
    } else {
        doExit();
    }
}

// ---------------------------------------------------------------- per-screen input

static int visibleRows(int rowH) {
    return kContentH / rowH;
}

void App::handleGames(Btn b) {
    int n = (int) catalog_.games.size();
    int vis = visibleRows(kRowH);
    if (b == Btn::Up) gamesList_.move(-1, n, vis);
    else if (b == Btn::Down) gamesList_.move(1, n, vis);
    else if (b == Btn::Left) gamesList_.move(-vis, n, vis);
    else if (b == Btn::Right) gamesList_.move(vis, n, vis);
    else if (b == Btn::A && n > 0) {
        gameIndex_ = gamesList_.sel;
        modsList_ = ListState();
        screen_ = Screen::Mods;
    } else if (b == Btn::Y) {
        startLoad();
    }
}

void App::handleMods(Btn b) {
    const Game *g = currentGame();
    if (!g) {
        screen_ = Screen::Games;
        return;
    }
    int n = (int) g->mods.size();
    int vis = visibleRows(kRowH + 16);
    if (b == Btn::Up) modsList_.move(-1, n, vis);
    else if (b == Btn::Down) modsList_.move(1, n, vis);
    else if (b == Btn::Left) modsList_.move(-vis, n, vis);
    else if (b == Btn::Right) modsList_.move(vis, n, vis);
    else if (b == Btn::B) screen_ = Screen::Games;
    else if (n > 0) {
        const Mod &m = g->mods[(size_t) modsList_.sel];
        if (b == Btn::A) openDetail(m.id);
        else if (b == Btn::Y) {
            if (!installer_.isInstalled(m.id) || updateAvailable(m)) requestInstall(*g, m);
        } else if (b == Btn::X && installer_.isInstalled(m.id)) {
            requestUninstall(m.id, m.name);
        }
    }
}

void App::handleDetail(Btn b) {
    const Game *g = nullptr;
    const Mod *m = catalog_.findMod(detailModId_, &g);
    if (!m || !g) {
        screen_ = detailReturn_;
        return;
    }
    JobQueue::View v;
    bool busy = jobs_->activeFor(m->id, v);
    bool installed = installer_.isInstalled(m->id);

    if (b == Btn::B) {
        screen_ = detailReturn_;
    } else if (b == Btn::Up) {
        detailScroll_ = std::max(0, detailScroll_ - 1);
    } else if (b == Btn::Down) {
        detailScroll_ = std::min(detailMaxScroll_, detailScroll_ + 1);
    } else if ((b == Btn::L || b == Btn::R) && detailReturn_ == Screen::Mods) {
        // Step through the game's mods without leaving the page.
        int idx = 0;
        for (size_t i = 0; i < g->mods.size(); ++i)
            if (g->mods[i].id == m->id) idx = (int) i;
        int n = (int) g->mods.size();
        idx = (idx + (b == Btn::R ? 1 : n - 1)) % n;
        modsList_.sel = idx;
        modsList_.clamp(n, visibleRows(kRowH + 16));
        detailModId_ = g->mods[(size_t) idx].id;
        detailScroll_ = 0;
    } else if (b == Btn::A) {
        if (busy) {
            screen_ = Screen::Queue;
        } else if (!installed || updateAvailable(*m)) {
            requestInstall(*g, *m);
        } else {
            Game gg{g->name, g->icon, g->titleIds, {}};
            const Mod mm = *m;
            choose("Reinstall", "Download and reinstall " + m->name + "?", {"Cancel", "Reinstall"},
                   [this, gg, mm](int i) {
                       if (i == 1) requestInstall(gg, mm);
                   });
        }
    } else if (b == Btn::X && installed && !busy) {
        requestUninstall(m->id, m->name);
    } else if (b == Btn::Y && installed && !busy) {
        toggleEnabled(m->id);
    } else if (b == Btn::X && busy) {
        jobs_->cancel(v.job);
    }
}

void App::handleInstalled(Btn b) {
    auto list = installer_.list();
    int n = (int) list.size();
    int vis = visibleRows(kRowH);
    if (b == Btn::Up) installedList_.move(-1, n, vis);
    else if (b == Btn::Down) installedList_.move(1, n, vis);
    else if (b == Btn::Left) installedList_.move(-vis, n, vis);
    else if (b == Btn::Right) installedList_.move(vis, n, vis);
    else if (n > 0) {
        installedList_.clamp(n, vis);
        const InstalledMod &im = list[(size_t) installedList_.sel];
        if (b == Btn::Y) toggleEnabled(im.id);
        else if (b == Btn::X) requestUninstall(im.id, im.name);
        else if (b == Btn::A) openDetail(im.id);
    }
}

void App::handleQueue(Btn b) {
    auto jobs = jobs_->snapshot();
    int n = (int) jobs.size();
    int vis = visibleRows(kRowH + 24);
    queueList_.clamp(n, vis);
    if (b == Btn::Up) queueList_.move(-1, n, vis);
    else if (b == Btn::Down) queueList_.move(1, n, vis);
    else if (b == Btn::Y) jobs_->clearFinished();
    else if (b == Btn::X && n > 0) jobs_->cancel(jobs[(size_t) queueList_.sel].job);
    else if (b == Btn::A && n > 0) {
        auto &v = jobs[(size_t) queueList_.sel];
        if (v.state == JobState::Failed || v.state == JobState::Cancelled) {
            // Retry.
            if (v.job->isCombo) {
                jobs_->enqueueCombo(v.job->mod.id, v.job->gameName, v.job->titleId);
            } else {
                const Game *g = nullptr;
                const Mod *m = catalog_.findMod(v.job->mod.id, &g);
                if (m) jobs_->enqueue(*m, v.job->gameName, v.job->titleId);
            }
        }
    }
}

void App::handleRepos(Btn b) {
    int n = (int) (catalog_.repos.size() + catalog_.warnings.size());
    int vis = visibleRows(36);
    if (b == Btn::Up) reposList_.move(-1, n, vis);
    else if (b == Btn::Down) reposList_.move(1, n, vis);
    else if (b == Btn::Y) startLoad();
}

// ---------------------------------------------------------------- drawing

void App::draw() {
    drawHeader();
    switch (screen_) {
    case Screen::Loading: drawLoading(); break;
    case Screen::Games: drawGames(); break;
    case Screen::Mods: drawMods(); break;
    case Screen::Detail: drawDetail(); break;
    case Screen::Installed: drawInstalled(); break;
    case Screen::Combos: drawCombos(); break;
    case Screen::ComboEdit: drawComboEdit(); break;
    case Screen::Queue: drawQueue(); break;
    case Screen::Repos: drawRepos(); break;
    }
    if (modal_.open) drawModal();
}

void App::drawHeader() {
    gfx_.fill(0, 0, Gfx::W, kHeaderH, color::Panel);
    gfx_.fill(0, kHeaderH - 3, Gfx::W, 3, color::Accent);
    int x = kMargin;
    x += gfx_.text("Patch", x, 14, FontSize::Title, color::Text);
    x += gfx_.text("Bay", x, 14, FontSize::Title, color::Accent);

    if (screen_ == Screen::Loading) return;

    const char *names[] = {"Games", "Installed", "Combos", "Downloads", "Repos"};
    Screen active = screen_;
    if (active == Screen::ComboEdit) active = Screen::Combos;
    if (active == Screen::Mods) active = Screen::Games;
    if (active == Screen::Detail) active = detailReturn_ == Screen::Installed ? Screen::Installed : Screen::Games;

    int tx = Gfx::W - kMargin;
    for (int i = kTabCount - 1; i >= 0; --i) {
        std::string label = names[i];
        if (kTabs[i] == Screen::Queue) {
            int n = jobs_->activeCount();
            if (n) label += " (" + std::to_string(n) + ")";
        }
        int w = gfx_.textWidth(label, FontSize::Normal) + 24;
        tx -= w;
        bool on = kTabs[i] == active;
        if (on) gfx_.fill(tx, 14, w, 44, color::PanelHi);
        gfx_.text(label, tx, 22, FontSize::Normal, on ? color::Accent : color::Dim, w, 1);
        tx -= 4;
    }
    gfx_.text("L", tx - 30, 22, FontSize::Normal, color::Dim);
    gfx_.text("R", Gfx::W - kMargin + 10, 22, FontSize::Normal, color::Dim);
}

void App::drawFooter(const std::string &hints) {
    int y = Gfx::H - kFooterH;
    gfx_.fill(0, y, Gfx::W, kFooterH, color::Panel);
    gfx_.text(hints, kMargin, y + 12, FontSize::Small, color::Dim, Gfx::W - 2 * kMargin);
}

void App::drawProgressBar(int x, int y, int w, int h, int64_t done, int64_t total, SDL_Color c) {
    gfx_.fill(x, y, w, h, color::PanelHi);
    if (total > 0) {
        int fw = (int) ((double) std::min(done, total) / (double) total * w);
        gfx_.fill(x, y, fw, h, c);
    } else {
        // Indeterminate: sliding block.
        int bw = w / 5;
        int pos = (int) ((SDL_GetTicks() / 4) % (uint32_t) (w + bw)) - bw;
        int bx = std::max(x, x + pos), bx2 = std::min(x + w, x + pos + bw);
        if (bx2 > bx) gfx_.fill(bx, y, bx2 - bx, h, c);
    }
}

void App::drawLoading() {
    std::string status;
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        status = loadStatus_;
    }
    gfx_.text("Loading mod repositories", 0, 300, FontSize::Large, color::Text, Gfx::W, 1);
    gfx_.text(status, 0, 350, FontSize::Normal, color::Dim, Gfx::W, 1);
    drawProgressBar(Gfx::W / 2 - 200, 400, 400, 8, 0, -1, color::Accent);
}

void App::drawGames() {
    drawFooter("A Open    Y Reload repos    L/R Tabs    + Exit");
    if (catalog_.games.empty()) {
        gfx_.text("No games found. See the Repos tab.", 0, 320, FontSize::Large, color::Dim, Gfx::W, 1);
        return;
    }
    int vis = visibleRows(kRowH);
    gamesList_.clamp((int) catalog_.games.size(), vis);
    auto installed = installer_.list();

    for (int r = 0; r < vis; ++r) {
        int i = gamesList_.scroll + r;
        if (i >= (int) catalog_.games.size()) break;
        const Game &g = catalog_.games[(size_t) i];
        int y = kContentY + r * kRowH;
        bool sel = i == gamesList_.sel;
        gfx_.fill(kMargin, y, Gfx::W - 2 * kMargin, kRowH - 6, sel ? color::PanelHi : color::Panel);
        if (sel) gfx_.fill(kMargin, y, 5, kRowH - 6, color::Accent);

        if (SDL_Texture *icon = images_->get(g.icon)) gfx_.image(icon, kMargin + 14, y + 5, 48, 48);
        else gfx_.fill(kMargin + 14, y + 5, 48, 48, color::Bg);

        bool onConsole = false;
        for (auto &t : g.titleIds) onConsole = onConsole || consoleTitles_.count(t);
        gfx_.text(g.name, kMargin + 80, y + 14, FontSize::Normal, onConsole ? color::Text : color::Dim, 700);

        int inst = 0;
        for (auto &im : installed)
            for (auto &m : g.mods)
                if (m.id == im.id) inst++;
        std::string right = std::to_string(g.mods.size()) + " mods";
        if (inst) right = std::to_string(inst) + " installed  |  " + right;
        gfx_.text(right, Gfx::W - kMargin - 380, y + 16, FontSize::Small, inst ? color::Good : color::Dim, 360, 2);
    }
}

void App::drawMods() {
    const Game *g = currentGame();
    if (!g) return;
    drawFooter("A Details    Y Install/Update    X Uninstall    B Back");

    gfx_.text(g->name, kMargin, kContentY, FontSize::Large, color::Text, 900);
    gfx_.text(join(g->titleIds, "  "), kMargin, kContentY + 42, FontSize::Small, color::Dim, 1000);

    const int top = kContentY + 80;
    const int rowH = kRowH + 16;
    int vis = (kContentH - 80) / rowH;
    modsList_.clamp((int) g->mods.size(), vis);

    for (int r = 0; r < vis; ++r) {
        int i = modsList_.scroll + r;
        if (i >= (int) g->mods.size()) break;
        const Mod &m = g->mods[(size_t) i];
        int y = top + r * rowH;
        bool sel = i == modsList_.sel;
        gfx_.fill(kMargin, y, Gfx::W - 2 * kMargin, rowH - 8, sel ? color::PanelHi : color::Panel);
        if (sel) gfx_.fill(kMargin, y, 5, rowH - 8, color::Accent);

        int tx = kMargin + 20;
        if (SDL_Texture *t = images_->get(m.thumbnail)) {
            gfx_.image(t, kMargin + 14, y + 4, 112, 63);
            tx = kMargin + 140;
        }
        std::string title = m.name + (m.isModpack() ? "  [modpack]" : "");
        gfx_.text(title, tx, y + 8, FontSize::Normal, color::Text, 700);
        std::string sub = "v" + m.version + "  by " + m.author + "  |  " + formatSize(m.fileSize);
        if (!m.tags.empty()) sub += "  |  " + join(m.tags, ", ");
        gfx_.text(sub, tx, y + 42, FontSize::Small, color::Dim, 800);

        SDL_Color sc = color::Dim;
        std::string st = statusLabel(m, sc);
        if (!st.empty()) gfx_.text(st, Gfx::W - kMargin - 260, y + 24, FontSize::Normal, sc, 240, 2);
    }
}

void App::drawDetail() {
    const Game *g = nullptr;
    const Mod *m = catalog_.findMod(detailModId_, &g);
    if (!m || !g) return;

    JobQueue::View v;
    bool busy = jobs_->activeFor(m->id, v);
    InstalledMod im;
    bool installed = installer_.get(m->id, im);
    bool update = updateAvailable(*m);

    std::string hints;
    if (busy) hints = "A View downloads    X Cancel    B Back";
    else if (!installed) hints = "A Install    B Back";
    else hints = std::string(update ? "A Update" : "A Reinstall") + "    X Uninstall    Y " +
                 (im.enabled ? "Disable" : "Enable") + "    B Back";
    if (detailReturn_ == Screen::Mods) hints += "    L/R Prev/Next";
    hints += "    Up/Down Scroll";
    drawFooter(hints);

    // Left column: media.
    const int lx = kMargin, ly = kContentY;
    const int mw = 480, mh = 270;
    gfx_.fill(lx, ly, mw, mh, color::Panel);
    std::string shot = m->thumbnail.empty() && !m->screenshots.empty() ? m->screenshots[0] : m->thumbnail;
    if (SDL_Texture *t = images_->get(shot)) gfx_.image(t, lx, ly, mw, mh);
    else gfx_.text(shot.empty() ? "No preview" : "Loading preview...", lx, ly + mh / 2 - 14, FontSize::Small,
                   color::Dim, mw, 1);

    int y = ly + mh + 16;
    auto field = [&](const std::string &k, const std::string &val) {
        if (val.empty()) return;
        gfx_.text(k, lx, y, FontSize::Small, color::Dim);
        gfx_.text(val, lx + 130, y, FontSize::Small, color::Text, mw - 130);
        y += 30;
    };
    field("Version", m->version + (installed && !im.version.empty() && im.version != m->version
                                       ? "  (installed " + im.version + ")"
                                       : ""));
    field("Author", m->author);
    field("Size", formatSize(m->fileSize));
    field("Released", m->releaseDate);
    field("License", m->license);
    field("Repo", m->repoName);
    if (installed) field("Installed", im.titleId + (im.enabled ? "" : "  (disabled)"));

    if (busy) {
        y += 6;
        std::string st = jobStateName(v.state);
        if (v.total > 0) st += "  " + formatSize(v.done) + " / " + formatSize(v.total);
        gfx_.text(st, lx, y, FontSize::Small, color::Accent2);
        drawProgressBar(lx, y + 30, mw, 10, v.done, v.total, color::Accent2);
    }

    // Right column: text, scrollable.
    const int rx = lx + mw + 32, rw = Gfx::W - rx - kMargin;
    gfx_.text(m->name, rx, ly, FontSize::Large, color::Text, rw);
    SDL_Color sc = color::Dim;
    std::string st = statusLabel(*m, sc);
    gfx_.text((m->isModpack() ? "Modpack" : "Mod") + std::string(" for ") + g->name, rx, ly + 44,
              FontSize::Small, color::Dim, rw - 200);
    if (!st.empty()) gfx_.text(st, rx, ly + 44, FontSize::Small, sc, rw, 2);

    std::vector<std::pair<std::string, SDL_Color>> lines;
    auto add = [&](const std::string &text, SDL_Color c) {
        for (auto &l : gfx_.wrap(text, FontSize::Small, rw)) lines.push_back({l, c});
    };
    add(m->description, color::Text);
    if (!m->tags.empty()) {
        lines.push_back({"", color::Text});
        add("Tags: " + join(m->tags, ", "), color::Dim);
    }
    if (!m->requirements.empty()) {
        lines.push_back({"", color::Text});
        add("Requires: " + join(m->requirements, ", "), color::Warn);
    }
    if (m->isModpack() && !m->includes.empty()) {
        std::vector<std::string> names;
        for (auto &id : m->includes) {
            const Mod *inc = catalog_.findMod(id);
            names.push_back(inc ? inc->name : id);
        }
        lines.push_back({"", color::Text});
        add("Includes: " + join(names, ", "), color::Accent2);
    }
    if (installed) {
        auto others = installer_.enabledForTitle(im.titleId, m->id);
        if (!others.empty() && im.enabled) {
            std::vector<std::string> names;
            for (auto &o : others) names.push_back(o.name);
            lines.push_back({"", color::Text});
            add("Also enabled for this game: " + join(names, ", ") +
                    ". SDCafiine will ask which to load at game start.",
                color::Warn);
        }
    }
    auto inCombos = installer_.combosContaining(m->id);
    if (!inCombos.empty()) {
        lines.push_back({"", color::Text});
        add("In combo: " + join(inCombos, ", "), color::Accent2);
    }
    if (!m->changelog.empty()) {
        lines.push_back({"", color::Text});
        add("Changelog", color::Accent);
        add(m->changelog, color::Dim);
    }

    const int textTop = ly + 90;
    const int lh = gfx_.lineHeight(FontSize::Small);
    int visLines = (kContentY + kContentH - textTop) / lh;
    detailMaxScroll_ = std::max(0, (int) lines.size() - visLines);
    detailScroll_ = std::min(detailScroll_, detailMaxScroll_);
    for (int i = 0; i < visLines; ++i) {
        int idx = detailScroll_ + i;
        if (idx >= (int) lines.size()) break;
        gfx_.text(lines[(size_t) idx].first, rx, textTop + i * lh, FontSize::Small, lines[(size_t) idx].second);
    }
    if (detailMaxScroll_ > 0) {
        int trackH = visLines * lh;
        int barH = std::max(20, trackH * visLines / (int) lines.size());
        int barY = textTop + (trackH - barH) * detailScroll_ / detailMaxScroll_;
        gfx_.fill(Gfx::W - kMargin + 12, textTop, 4, trackH, color::Panel);
        gfx_.fill(Gfx::W - kMargin + 12, barY, 4, barH, color::Accent);
    }
}

void App::drawInstalled() {
    drawFooter("A Details    Y Enable/Disable    X Uninstall    L/R Tabs");
    auto list = installer_.list();
    if (list.empty()) {
        gfx_.text("No mods installed yet.", 0, 320, FontSize::Large, color::Dim, Gfx::W, 1);
        return;
    }
    int vis = visibleRows(kRowH);
    installedList_.clamp((int) list.size(), vis);
    for (int r = 0; r < vis; ++r) {
        int i = installedList_.scroll + r;
        if (i >= (int) list.size()) break;
        const InstalledMod &im = list[(size_t) i];
        int y = kContentY + r * kRowH;
        bool sel = i == installedList_.sel;
        gfx_.fill(kMargin, y, Gfx::W - 2 * kMargin, kRowH - 6, sel ? color::PanelHi : color::Panel);
        if (sel) gfx_.fill(kMargin, y, 5, kRowH - 6, color::Accent);

        gfx_.text(im.name, kMargin + 20, y + 6, FontSize::Normal, im.enabled ? color::Text : color::Dim, 640);
        std::string sub = im.gameName + "  |  " + im.titleId + "  |  " +
                          (im.version.empty() ? std::string("version unknown") : "v" + im.version);
        auto inCombos = installer_.combosContaining(im.id);
        if (!inCombos.empty()) sub += " | in " + join(inCombos, ", ");
        gfx_.text(sub, kMargin + 20, y + 36, FontSize::Small, color::Dim, 760);

        const Mod *m = catalog_.findMod(im.id);
        SDL_Color c = im.enabled ? color::Good : color::Dim;
        std::string st = im.enabled ? "Enabled" : "Disabled";
        if (m && updateAvailable(*m)) {
            st = "Update " + m->version;
            c = color::Warn;
        }
        gfx_.text(st, Gfx::W - kMargin - 300, y + 18, FontSize::Normal, c, 280, 2);
    }
}

void App::drawQueue() {
    drawFooter("A Retry failed    X Cancel    Y Clear finished    L/R Tabs");
    auto jobs = jobs_->snapshot();
    if (jobs.empty()) {
        gfx_.text("No downloads.", 0, 320, FontSize::Large, color::Dim, Gfx::W, 1);
        return;
    }
    const int rowH = kRowH + 24;
    int vis = visibleRows(rowH);
    queueList_.clamp((int) jobs.size(), vis);
    for (int r = 0; r < vis; ++r) {
        int i = queueList_.scroll + r;
        if (i >= (int) jobs.size()) break;
        const auto &v = jobs[(size_t) i];
        int y = kContentY + r * rowH;
        bool sel = i == queueList_.sel;
        gfx_.fill(kMargin, y, Gfx::W - 2 * kMargin, rowH - 8, sel ? color::PanelHi : color::Panel);
        if (sel) gfx_.fill(kMargin, y, 5, rowH - 8, color::Accent);

        gfx_.text(v.job->mod.name, kMargin + 20, y + 6, FontSize::Normal, color::Text, 700);
        SDL_Color c = v.state == JobState::Done     ? color::Good
                      : v.state == JobState::Failed ? color::Bad
                      : v.state == JobState::Cancelled ? color::Dim
                                                       : color::Accent2;
        std::string st = jobStateName(v.state);
        if (v.state == JobState::Failed || v.state == JobState::Cancelled) {
            if (!v.error.empty() && v.error != "Cancelled") st += ": " + v.error;
        } else if (v.state != JobState::Done && v.state != JobState::Queued && v.total > 0) {
            st += "  " + formatSize(v.done) + " / " + formatSize(v.total);
        }
        gfx_.text(st, kMargin + 20, y + 38, FontSize::Small, c, 900);
        if (v.state == JobState::Downloading || v.state == JobState::Verifying || v.state == JobState::Extracting ||
            v.state == JobState::Building)
            drawProgressBar(kMargin + 20, y + rowH - 22, Gfx::W - 2 * kMargin - 40, 6, v.done, v.total, c);
        gfx_.text(v.job->gameName, Gfx::W - kMargin - 320, y + 10, FontSize::Small, color::Dim, 300, 2);
    }
}

void App::drawRepos() {
    drawFooter("Y Reload    L/R Tabs    + Exit");
    int y = kContentY;
    gfx_.text("Config: " + (config_.source.empty() ? paths::kConfigFile : config_.source), kMargin, y,
              FontSize::Small, color::Dim, Gfx::W - 2 * kMargin);
    y += 34;

    if (!config_.error.empty()) {
        gfx_.text(config_.error, kMargin, y, FontSize::Normal, color::Bad);
        y += 40;
    }
    if (config_.repos.empty()) {
        std::vector<std::string> help = {
            "No repositories configured.",
            "",
            "Edit SD:/wiiu/apps/patchbay/config.json on your SD card:",
            "",
            "{",
            "  \"repos\": [",
            "    \"https://raw.githubusercontent.com/<user>/<repo>/main/repo.json\"",
            "  ]",
            "}",
            "",
            "CoffeeShop repos and config files work unchanged.",
        };
        for (auto &l : help) {
            gfx_.text(l, kMargin, y, FontSize::Normal, color::Text);
            y += 34;
        }
        return;
    }

    struct Line {
        std::string a, b;
        SDL_Color c;
    };
    std::vector<Line> lines;
    for (auto &r : catalog_.repos) {
        if (r.error.empty())
            lines.push_back({r.name + "  (" + std::to_string(r.gameCount) + " games, " + std::to_string(r.modCount) +
                                 " mods)",
                             r.url, color::Good});
        else
            lines.push_back({"Failed: " + r.error, r.url, color::Bad});
    }
    for (auto &w : catalog_.warnings) lines.push_back({w, "", color::Warn});

    const int rowH = 36;
    int vis = (kContentY + kContentH - y) / rowH;
    reposList_.clamp((int) lines.size(), vis / 2);
    for (int i = reposList_.scroll; i < (int) lines.size() && y < kContentY + kContentH - rowH; ++i) {
        bool sel = i == reposList_.sel;
        if (sel) gfx_.fill(kMargin - 10, y - 2, 6, rowH - 4, color::Accent);
        gfx_.text(lines[(size_t) i].a, kMargin, y, FontSize::Normal, lines[(size_t) i].c, Gfx::W - 2 * kMargin);
        y += rowH;
        if (!lines[(size_t) i].b.empty()) {
            gfx_.text(lines[(size_t) i].b, kMargin + 20, y - 6, FontSize::Small, color::Dim, Gfx::W - 2 * kMargin - 20);
            y += 28;
        }
    }
}

void App::drawModal() {
    gfx_.fill(0, 0, Gfx::W, Gfx::H, SDL_Color{0, 0, 0, 170});
    const int w = 720;
    const int lh = gfx_.lineHeight(FontSize::Normal);
    int optH = modal_.options.empty() ? 60 : (int) modal_.options.size() * 52;
    int h = 90 + (int) modal_.body.size() * lh + optH + 30;
    int x = (Gfx::W - w) / 2, y = (Gfx::H - h) / 2;
    gfx_.fill(x, y, w, h, color::Panel);
    gfx_.outline(x, y, w, h, color::Accent, 2);
    gfx_.text(modal_.title, x + 40, y + 24, FontSize::Large, color::Accent, w - 80);

    int ty = y + 80;
    for (auto &l : modal_.body) {
        gfx_.text(l, x + 40, ty, FontSize::Normal, color::Text);
        ty += lh;
    }
    ty += 16;
    if (modal_.options.empty()) {
        gfx_.fill(x + w / 2 - 80, ty, 160, 44, color::PanelHi);
        gfx_.text("OK", x + w / 2 - 80, ty + 8, FontSize::Normal, color::Accent, 160, 1);
        return;
    }
    for (size_t i = 0; i < modal_.options.size(); ++i) {
        bool sel = (int) i == modal_.sel;
        gfx_.fill(x + 40, ty, w - 80, 44, sel ? color::PanelHi : color::Bg);
        if (sel) gfx_.fill(x + 40, ty, 5, 44, color::Accent);
        gfx_.text(modal_.options[i], x + 60, ty + 8, FontSize::Normal, sel ? color::Text : color::Dim, w - 120);
        ty += 52;
    }
}

// ---------------------------------------------------------------- combos

void App::newCombo() {
    // Titles with at least two installed mods.
    std::vector<std::string> tids, names;
    std::map<std::string, std::pair<std::string, int>> byTitle; // titleId -> (game name, count)
    for (auto &im : installer_.list()) {
        auto &e = byTitle[im.titleId];
        if (e.first.empty()) e.first = im.gameName;
        e.second++;
    }
    for (auto &kv : byTitle) {
        if (kv.second.second < 2) continue;
        tids.push_back(kv.first);
        names.push_back(kv.second.first);
    }
    if (tids.empty()) {
        message("New combo", "Install at least two mods for the same game first.");
        return;
    }
    auto start = [this, tids, names](int i) {
        if (i < 0 || i >= (int) tids.size()) return;
        Combo c;
        c.titleId = tids[(size_t) i];
        c.gameName = names[(size_t) i];
        c.id = installer_.nextComboId(c.titleId);
        openComboEditor(c, true);
    };
    if (tids.size() == 1) {
        start(0);
        return;
    }
    std::vector<std::string> labels;
    for (size_t i = 0; i < tids.size(); ++i) labels.push_back(names[i] + " (" + tids[i] + ")");
    choose("New combo", "Combine mods for which game?", labels, start);
}

void App::openComboEditor(const Combo &combo, bool isNew) {
    comboEdit_ = combo;
    comboIsNew_ = isNew;
    comboDirty_ = isNew;
    comboEditList_ = ListState();
    comboPlanNeeded_ = true;
    screen_ = Screen::ComboEdit;
}

std::vector<App::ComboRow> App::comboEditRows() const {
    std::vector<ComboRow> rows;
    for (size_t i = 0; i < comboEdit_.members.size(); ++i) {
        const std::string &id = comboEdit_.members[i];
        InstalledMod im;
        rows.push_back({id, installer_.get(id, im) ? im.name : id, (int) i});
    }
    for (auto &im : installer_.list()) {
        if (im.titleId != comboEdit_.titleId) continue;
        if (std::find(comboEdit_.members.begin(), comboEdit_.members.end(), im.id) != comboEdit_.members.end())
            continue;
        rows.push_back({im.id, im.name, -1});
    }
    return rows;
}

void App::selectComboRow(const std::string &modId) {
    auto rows = comboEditRows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].modId == modId) comboEditList_.sel = (int) i;
    comboEditList_.clamp((int) rows.size(), (kContentH - 80) / kRowH);
}

void App::updateComboPlan() {
    if (!comboPlanNeeded_) return;
    uint32_t gen;
    {
        std::lock_guard<std::mutex> lock(planMutex_);
        if (planRunning_) return; // start again once the current run finishes
        planRunning_ = true;
        gen = ++planGen_;
    }
    comboPlanNeeded_ = false;
    planWorker_.reset(); // previous run already finished, joins at once

    std::string tid = comboEdit_.titleId;
    std::vector<std::string> members = comboEdit_.members;
    auto work = [this, gen, tid, members] {
        ComboPlan p = installer_.planCombo(tid, members, &planCancel_);
        int files = (int) p.copies.size();
        p.copies.clear();
        p.copies.shrink_to_fit();
        std::lock_guard<std::mutex> lock(planMutex_);
        planRunning_ = false;
        if (gen != planGen_) return;
        plan_ = std::move(p);
        planFiles_ = files;
        planResultGen_ = gen;
    };
    planWorker_.reset(new Worker(work));
    if (!planWorker_->ok()) {
        // No thread available. Walking big mods here would freeze the UI, so
        // skip the preview; the build itself still works.
        planWorker_.reset();
        std::lock_guard<std::mutex> lock(planMutex_);
        planRunning_ = false;
    }
}

void App::saveAndBuildCombo() {
    if (comboEdit_.members.size() < 2) {
        message("Combo", "A combo needs at least two mods.");
        return;
    }
    JobQueue::View v;
    if (jobs_->activeFor(comboEdit_.id, v)) {
        message("Combo", comboEdit_.id + " is already being built.");
        return;
    }
    installer_.saveComboDefinition(comboEdit_);
    jobs_->enqueueCombo(comboEdit_.id, comboEdit_.gameName, comboEdit_.titleId);
    comboDirty_ = false;
    comboIsNew_ = false;

    auto list = installer_.combos();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].id == comboEdit_.id) combosList_.sel = (int) i;
    combosList_.clamp((int) list.size(), visibleRows(kRowH));
    screen_ = Screen::Combos;
    message("Building combo", comboEdit_.id + " was added to the download queue. Member mods are disabled once it "
                                              "is built so SDCafiine loads only the combo.");
}

void App::handleCombos(Btn b) {
    auto list = installer_.combos();
    int n = (int) list.size();
    int vis = visibleRows(kRowH);
    combosList_.clamp(n, vis);
    if (b == Btn::Up) combosList_.move(-1, n, vis);
    else if (b == Btn::Down) combosList_.move(1, n, vis);
    else if (b == Btn::Left) combosList_.move(-vis, n, vis);
    else if (b == Btn::Right) combosList_.move(vis, n, vis);
    else if (b == Btn::Minus) newCombo();
    else if (n > 0) {
        const Combo c = list[(size_t) combosList_.sel];
        JobQueue::View v;
        bool busy = jobs_->activeFor(c.id, v);
        if (b == Btn::A) {
            openComboEditor(c, false);
        } else if (b == Btn::Y) {
            if (busy) {
                message("Combo", c.id + " is being built. Try again when it is done.");
                return;
            }
            std::string err;
            if (!installer_.setComboEnabled(c.id, !c.enabled, err)) message("Error", err);
        } else if (b == Btn::X) {
            if (busy) {
                message("Combo", c.id + " is being built. Cancel it from the Downloads tab first.");
                return;
            }
            std::string id = c.id;
            choose("Delete combo", "Delete " + id + " from the SD card? Member mods are kept.", {"Cancel", "Delete"},
                   [this, id](int i) {
                       if (i != 1) return;
                       std::string err;
                       if (!installer_.deleteCombo(id, err)) message("Delete failed", err);
                   });
        }
    }
}

void App::handleComboEdit(Btn b) {
    auto rows = comboEditRows();
    int n = (int) rows.size();
    int vis = (kContentH - 80) / kRowH;
    comboEditList_.clamp(n, vis);
    auto &members = comboEdit_.members;

    if (b == Btn::Up) comboEditList_.move(-1, n, vis);
    else if (b == Btn::Down) comboEditList_.move(1, n, vis);
    else if (b == Btn::Left) comboEditList_.move(-vis, n, vis);
    else if (b == Btn::Right) comboEditList_.move(vis, n, vis);
    else if (b == Btn::Minus) saveAndBuildCombo();
    else if (b == Btn::B) {
        if (!comboDirty_) {
            screen_ = Screen::Combos;
            return;
        }
        choose("Combo", "Discard changes?", {"Keep editing", "Discard"}, [this](int i) {
            if (i != 1) return;
            comboDirty_ = false;
            screen_ = Screen::Combos;
        });
    } else if (n > 0) {
        const ComboRow row = rows[(size_t) comboEditList_.sel];
        int mi = row.memberIndex;
        bool changed = false;
        if (b == Btn::A) {
            if (mi >= 0) members.erase(members.begin() + mi);
            else members.push_back(row.modId);
            changed = true;
        } else if (b == Btn::X && mi > 0) {
            std::swap(members[(size_t) mi], members[(size_t) mi - 1]);
            changed = true;
        } else if (b == Btn::Y && mi >= 0 && mi + 1 < (int) members.size()) {
            std::swap(members[(size_t) mi], members[(size_t) mi + 1]);
            changed = true;
        }
        if (changed) {
            comboDirty_ = true;
            comboPlanNeeded_ = true;
            selectComboRow(row.modId);
        }
    }
}

void App::drawCombos() {
    auto list = installer_.combos();
    int vis = visibleRows(kRowH);
    combosList_.clamp((int) list.size(), vis);

    std::string hints = "A Edit";
    if (!list.empty()) {
        const Combo &c = list[(size_t) combosList_.sel];
        if (c.built && installer_.comboStale(c)) hints = "A Edit/Rebuild";
    }
    drawFooter(hints + "    Y Enable/Disable    X Delete    - New combo    L/R Tabs    + Exit");

    if (list.empty()) {
        gfx_.text("No combos. Press - to combine installed mods for a game.", 0, 320, FontSize::Large, color::Dim,
                  Gfx::W, 1);
        return;
    }
    for (int r = 0; r < vis; ++r) {
        int i = combosList_.scroll + r;
        if (i >= (int) list.size()) break;
        const Combo &c = list[(size_t) i];
        int y = kContentY + r * kRowH;
        bool sel = i == combosList_.sel;
        gfx_.fill(kMargin, y, Gfx::W - 2 * kMargin, kRowH - 6, sel ? color::PanelHi : color::Panel);
        if (sel) gfx_.fill(kMargin, y, 5, kRowH - 6, color::Accent);

        gfx_.text(c.id, kMargin + 20, y + 6, FontSize::Normal, c.enabled ? color::Text : color::Dim, 640);
        std::string sub = c.gameName + "  |  " + c.titleId + "  |  " + std::to_string(c.members.size()) + " mods";
        if (c.built) sub += "  |  " + formatSize(c.sizeBytes);
        gfx_.text(sub, kMargin + 20, y + 36, FontSize::Small, color::Dim, 760);

        SDL_Color sc = color::Dim;
        std::string st;
        JobQueue::View v;
        if (jobs_->activeFor(c.id, v)) {
            sc = color::Accent2;
            if (v.state == JobState::Queued) {
                st = "Queued";
            } else {
                int pct = v.total > 0 ? (int) (v.done * 100 / v.total) : 0;
                st = std::string(jobStateName(v.state)) + " " + std::to_string(pct) + "%";
            }
        } else if (!c.built) {
            sc = color::Warn;
            st = "Not built";
        } else if (installer_.comboStale(c)) {
            sc = color::Warn;
            st = "Needs rebuild";
        } else if (c.enabled) {
            sc = color::Good;
            st = "Enabled";
        } else {
            st = "Disabled";
        }
        gfx_.text(st, Gfx::W - kMargin - 300, y + 18, FontSize::Normal, sc, 280, 2);
    }
}

void App::drawComboEdit() {
    updateComboPlan();
    drawFooter("A Add/Remove    X Move up    Y Move down    - Save & build    B Back");

    const int panelW = 420;
    const int listW = Gfx::W - 2 * kMargin - panelW - 24;
    gfx_.text(comboEdit_.id + (comboIsNew_ ? "  (new)" : "") + (comboDirty_ ? "  *" : ""), kMargin, kContentY,
              FontSize::Large, color::Text, listW);
    gfx_.text(comboEdit_.gameName + "  |  " + comboEdit_.titleId, kMargin, kContentY + 42, FontSize::Small,
              color::Dim, listW);

    const int top = kContentY + 80;
    int vis = (kContentH - 80) / kRowH;
    auto rows = comboEditRows();
    comboEditList_.clamp((int) rows.size(), vis);
    if (rows.empty())
        gfx_.text("No installed mods for this game.", kMargin, top + 20, FontSize::Normal, color::Dim, listW);
    for (int r = 0; r < vis; ++r) {
        int i = comboEditList_.scroll + r;
        if (i >= (int) rows.size()) break;
        const ComboRow &row = rows[(size_t) i];
        int y = top + r * kRowH;
        bool sel = i == comboEditList_.sel;
        bool member = row.memberIndex >= 0;
        gfx_.fill(kMargin, y, listW, kRowH - 6, sel ? color::PanelHi : color::Panel);
        if (sel) gfx_.fill(kMargin, y, 5, kRowH - 6, color::Accent);
        if (member)
            gfx_.text("#" + std::to_string(row.memberIndex + 1), kMargin + 16, y + 14, FontSize::Normal,
                      color::Accent, 60);
        gfx_.text(row.name, kMargin + 80, y + 6, FontSize::Normal, member ? color::Text : color::Dim, listW - 100);
        gfx_.text(member ? "In combo" : "Not included", kMargin + 80, y + 36, FontSize::Small,
                  member ? color::Good : color::Dim, listW - 100);
    }

    // Right panel: merge preview.
    const int px = Gfx::W - kMargin - panelW, py = kContentY;
    const int ph = kContentH;
    gfx_.fill(px, py, panelW, ph, color::Panel);
    const int x = px + 20, w = panelW - 40;
    int y = py + 16;
    gfx_.text("Preview", x, y, FontSize::Normal, color::Accent, w);
    y += 44;

    bool ready;
    ComboPlan plan;
    int files = 0;
    {
        std::lock_guard<std::mutex> lock(planMutex_);
        ready = !comboPlanNeeded_ && planResultGen_ == planGen_;
        if (ready) {
            plan = plan_;
            files = planFiles_;
        }
    }
    gfx_.text("Members: " + std::to_string(comboEdit_.members.size()), x, y, FontSize::Small, color::Text, w);
    y += 30;
    if (!ready) {
        gfx_.text("Calculating...", x, y, FontSize::Small, color::Dim, w);
        return;
    }
    gfx_.text("Total size: " + formatSize(plan.totalBytes) + "  (" + std::to_string(files) + " files)", x, y,
              FontSize::Small, color::Text, w);
    y += 30;
    gfx_.text("Conflicts: " + std::to_string(plan.conflicts) + " files", x, y, FontSize::Small,
              plan.conflicts > 0 ? color::Warn : color::Text, w);
    y += 30;

    const int lh = gfx_.lineHeight(FontSize::Small);
    const int bottom = py + ph - 16;
    for (auto &l : plan.missing.empty() ? std::vector<std::string>()
                                        : gfx_.wrap("Not installed: " + join(plan.missing, ", "), FontSize::Small, w)) {
        if (y + lh > bottom) break;
        gfx_.text(l, x, y, FontSize::Small, color::Bad, w);
        y += lh;
    }

    auto note = gfx_.wrap("Higher in the list wins when two mods replace the same file.", FontSize::Small, w);
    int noteY = bottom - (int) note.size() * lh;
    y += 6;
    for (size_t i = 0; i < plan.conflictSamples.size() && i < 12; ++i) {
        if (y + lh > noteY - 6) break;
        gfx_.text(plan.conflictSamples[i], x, y, FontSize::Small, color::Dim, w);
        y += lh;
    }
    for (auto &l : note) {
        gfx_.text(l, x, noteY, FontSize::Small, color::Dim, w);
        noteY += lh;
    }
}
