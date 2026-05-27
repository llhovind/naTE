#include "app/App.h"
#include "config/ColorScheme.h"
#include "db/JsonConnectionRepository.h"
#include "db/JsonNamedWorkspaceRepository.h"
#include "db/JsonSessionRestoreRepository.h"
#include "session/AppSessionDefaults.h"
#include "session/RestoreState.h"
#include "ui/MainFrame.h"
#include "ui/TerminalTile.h"
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <libssh2.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

static std::atomic<bool> g_sigtermReceived{false};

namespace {
    std::string NateDir() {
        const char* home = std::getenv("HOME");
        return home ? std::string(home) + "/.nate" : std::string(".nate");
    }

    std::string LockPath(int n) {
        return NateDir() + "/instance-" + std::to_string(n) + ".lock";
    }

    bool TryClaimSlot(int n) {
        const std::string path = LockPath(n);

        // Check for a stale lock from a dead process.
        std::ifstream in(path);
        if (in.is_open()) {
            pid_t pid = 0;
            in >> pid;
            in.close();
            if (pid > 0 && kill(pid, 0) == 0)
                return false; // process is alive — slot is taken
            // Stale lock: fall through and overwrite.
        }

        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open())
            return false;
        out << getpid();
        return true;
    }
} // namespace

int App::AcquireInstanceId() {
    mkdir(NateDir().c_str(), 0700);
    for (int n = 0; n < 32; ++n) {
        if (TryClaimSlot(n))
            return n;
    }
    return 32; // all slots taken — degenerate but non-crashing
}

void App::ReleaseInstanceId(int id) {
    if (id < 32)
        std::remove(LockPath(id).c_str());
}

bool App::OnInit() {
    m_instanceId = AcquireInstanceId();
    wxImage::AddHandler(new wxPNGHandler());
    libssh2_init(0);

    const wxString exeDir =
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();

    m_configPath = NateDir() + "/config.ini";
    m_themesDir  = NateDir() + "/themes";
    mkdir(m_themesDir.c_str(), 0700);

    // Resolve factory data directory.
    // Dev/portable build: config.ini and themes/ sit next to the executable.
    // Installed build (AppImage, DEB, RPM): binary is in bin/, data is in ../share/nate/.
    const std::string factoryDataDir = [&]() -> std::string {
        const std::string adj = exeDir.ToStdString();
        if (std::ifstream(adj + "/config.ini").is_open())
            return adj;
        return adj + "/../share/nate";
    }();

    // Seed user config from factory default on first run.
    if (!std::ifstream(m_configPath).is_open()) {
        const std::string factoryPath = factoryDataDir + "/config.ini";
        if (std::ifstream src{factoryPath, std::ios::binary}) {
            std::ofstream dst{m_configPath, std::ios::binary | std::ios::trunc};
            dst << src.rdbuf();
        }
    }

    // Copy any factory theme files not yet present in the user themes dir.
    {
        const std::string factoryThemes = factoryDataDir + "/themes";
        for (const auto& scheme : ColorScheme::scanDirectory(factoryThemes)) {
            const std::string dst = m_themesDir + "/" + scheme.stem + ".ini";
            if (!std::ifstream(dst).is_open())
                scheme.saveToFile(dst);
        }
    }

    m_cfg = AppConfig::load(m_configPath, m_themesDir);

    const std::string connectionsPath = [] {
        const char* home = std::getenv("HOME");
        const std::string base = home ? std::string(home) + "/.nate" : std::string(".nate");
        return base + "/connections.json";
    }();
    m_connectionStore = std::make_unique<term::db::ConnectionStore>(
        std::make_unique<term::db::JsonConnectionRepository>(connectionsPath));

    m_sessionManager = std::make_unique<term::session::SessionManager>();

    const std::string restorePath = NateDir() + "/session-restore.json";
    m_restoreRepo = std::make_unique<term::db::JsonSessionRestoreRepository>(restorePath);
    m_namedRepo   = std::make_unique<term::db::JsonNamedWorkspaceRepository>(NateDir() + "/workspaces");

    // Parse --no-restore / --restore-last-workspace CLI flags.
    // --no-restore:              suppress restore even when config enables it.
    // --restore-last-workspace:  force restore even when config disables it.
    // --no-restore takes precedence if both are supplied.
    bool noRestoreFlag    = false;
    bool forceRestoreFlag = false;
    for (int i = 1; i < argc; ++i) {
        const auto arg = argv[i].ToStdString();
        if (arg == "--no-restore")               { noRestoreFlag    = true; }
        else if (arg == "--restore-last-workspace") { forceRestoreFlag = true; }
    }

    const bool autoRestore = (m_cfg.autoRestoreSession || forceRestoreFlag)
                             && !noRestoreFlag
                             && m_restoreRepo->HasSnapshot();
    if (!autoRestore) {
        CreateNewWindow();
    } else {
        auto state = m_restoreRepo->Load();
        RestoreStateImpl(state, nullptr);
        m_restoreRepo->Delete();
    }

    if (m_cfg.sessionSaveInterval > 0) {
        m_saveTimer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            SaveRestoreState();
        });
        m_saveTimer.Start(m_cfg.sessionSaveInterval * 1000);
    }

    Bind(wxEVT_QUERY_END_SESSION, &App::OnQueryEndSession, this);
    Bind(wxEVT_END_SESSION,       &App::OnEndSession,       this);
    Bind(wxEVT_IDLE,              &App::OnIdle,             this);

    std::signal(SIGTERM, [](int) {
        g_sigtermReceived.store(true, std::memory_order_relaxed);
        wxWakeUpIdle();
    });

    return true;
}

MainFrame* App::CreateNewWindow()
{
    auto wc = std::make_unique<WindowContext>();
    wc->router   = std::make_unique<term::input::InputRouter>();
    wc->windowId = m_nextWindowId++;

    auto* frame = new MainFrame(m_cfg, *wc->router, *m_connectionStore);
    wc->frame = frame;

    wc->uiManager = std::make_unique<ui::UIManager>(
        *m_sessionManager, frame, m_cfg,
        *wc->router, frame->GetEditMenu());

    frame->SetUIManager(wc->uiManager.get());

    wc->uiManager->SetOnGridEmptyCallback([this, mgr = wc->uiManager.get(), frame]() {
        if (m_globalCloseInProgress)
            return; // CloseAllSessionsGlobal handles window teardown explicitly
        if (m_windows.size() > 1)
            frame->CallAfter([this, mgr, frame]() {
                // Re-check: a same-window session move temporarily empties
                // sessions_ between ReleaseSession and TakeSession. By the
                // time this deferred close runs, TakeSession has already
                // re-populated sessions_, so abort the close in that case.
                if (!mgr->HasAnySessions())
                    frame->Close();
            });
    });

    wc->uiManager->SetSessionListChangedCallback([this]() {
        CallAfter([this]() { RebuildWindowMenus(); });
    });

    wc->uiManager->SetOnBeforeCloseCallback([this]() {
        // Only save when this is the last window being closed (QuitAll handles
        // the multi-window case by calling SaveRestoreState() before Close()).
        if (m_windows.size() == 1)
            SaveRestoreState();
    });

    frame->Bind(wxEVT_DESTROY, [this, frame](wxWindowDestroyEvent& evt) {
        if (evt.GetEventObject() == frame) {
            auto it = std::find_if(m_windows.begin(), m_windows.end(),
                [frame](const std::unique_ptr<WindowContext>& w) {
                    return w->frame == frame;
                });
            if (it != m_windows.end())
                m_windows.erase(it);
            RebuildWindowMenus();
        }
        evt.Skip();
    });

    frame->Show();
    frame->SetTitle(wxString::Format("naTE %d:%d", m_instanceId, wc->windowId));
    m_windows.push_back(std::move(wc));

    RebuildWindowMenus();
    return frame;
}

term::session::SessionId App::CreateSessionInWindow(
    const term::session::Connection& conn, MainFrame* target)
{
    return CreateSessionInTile(conn, target, nullptr);
}

term::session::SessionId App::CreateSessionInTile(
    const term::session::Connection& conn,
    MainFrame*    target,
    TerminalTile* targetTile)
{
    WindowContext* ctx = FindContext(target);
    if (!ctx) return 0;

    const unsigned short cols = conn.columnWidth
        ? conn.columnWidth
        : static_cast<unsigned short>(m_cfg.columns);

    const unsigned short rows = conn.rows
        ? conn.rows
        : static_cast<unsigned short>(m_cfg.rows);

    term::session::AppSessionDefaults appDefaults;
    appDefaults.workingDir   = m_cfg.defaultWorkingDir;
    appDefaults.envVars      = m_cfg.defaultEnvVars;
    appDefaults.envFilePath  = m_cfg.defaultEnvFilePath;
    appDefaults.loginShell   = m_cfg.defaultLoginShell;

    term::session::SessionId id = 0;
    try {
        id = m_sessionManager->CreateSession(
            conn,
            m_cfg.scrollbackLines,
            cols,
            rows,
            std::move(appDefaults),
            static_cast<unsigned short>(m_cfg.ptyLineWidth));
    } catch (const std::exception& e) {
        wxMessageBox(wxString::FromUTF8(e.what()), "Connection Failed",
                     wxOK | wxICON_ERROR, target);
        return 0;
    }

    m_sessionManager->SetSessionObserver(id, ctx->uiManager.get());
    m_sessionManager->RegisterRouter(id, *ctx->router);
    ctx->uiManager->TakeSession(
        id,
        m_sessionManager->MakeTitleGetter(id, conn.profileTitle, conn.useProfileTitle),
        cols,
        rows,
        conn.label,
        targetTile);
    return id;
}


void App::QuitAll()
{
    if (!m_sessionManagerShutdown) {
        int totalSessions = 0;
        for (auto& w : m_windows)
            if (w->uiManager)
                totalSessions += static_cast<int>(w->uiManager->GetSessionList().size());

        if (totalSessions > 0) {
            const int nw = static_cast<int>(m_windows.size());
            const wxString msg = wxString::Format(
                "Closing all windows will end %d session%s across %d window%s. "
                "Any unsaved work may be lost.\n\nClose all?",
                totalSessions, totalSessions == 1 ? "" : "s",
                nw, nw == 1 ? "" : "s");
            MainFrame* parent = m_windows.empty() ? nullptr : m_windows.front()->frame;
            if (wxMessageBox(msg, "Confirm Close All",
                             wxYES_NO | wxICON_WARNING, parent) != wxYES)
                return;
        }
    }

    // Save restore state before any windows are closed (the single-window path
    // is handled by the before-close callback; QuitAll must do it here).
    SaveRestoreState();

    std::vector<MainFrame*> frames;
    frames.reserve(m_windows.size());
    for (auto& w : m_windows)
        frames.push_back(w->frame);
    for (auto* f : frames)
        f->Close(true);  // force=true: OnClose will not re-prompt
}

void App::CloseAllSessionsGlobal(MainFrame* callerFrame)
{
    int totalSessions = 0;
    for (auto& w : m_windows)
        if (w->uiManager)
            totalSessions += static_cast<int>(w->uiManager->GetSessionList().size());

    if (totalSessions == 0)
        return;

    const wxString msg = wxString::Format(
        "This will close %d session%s. Any unsaved work may be lost.\n\nContinue?",
        totalSessions, totalSessions == 1 ? "" : "s");
    if (wxMessageBox(msg, "Close All Sessions",
                     wxYES_NO | wxICON_WARNING, callerFrame) != wxYES)
        return;

    // Snapshot non-anchor frames before any teardown modifies m_windows.
    // Anchor = oldest window (index 0); it keeps its frame open after sessions close.
    MainFrame* anchor = m_windows.front()->frame;
    std::vector<MainFrame*> toClose;
    toClose.reserve(m_windows.size() - 1);
    for (auto& w : m_windows)
        if (w->frame != anchor)
            toClose.push_back(w->frame);

    // Suppress onGridEmptyCb_ auto-close for the duration of session teardown
    // so we control exactly which windows close and which stays open.
    m_globalCloseInProgress = true;
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it)
        if ((*it)->uiManager)
            (*it)->uiManager->CloseAllSessions();
    m_globalCloseInProgress = false;

    // Explicitly close every non-anchor window. Sessions are already gone so
    // OnClose will not prompt; it will just proceed with destruction.
    for (auto* f : toClose)
        f->Close();
}

void App::OnQueryEndSession(wxCloseEvent& event)
{
    m_sessionManagerShutdown = true;
    SaveRestoreState();
    event.Skip(); // do not veto — allow logout to proceed
}

void App::OnEndSession(wxCloseEvent& event)
{
    m_sessionManagerShutdown = true;
    QuitAll();
    event.Skip();
}

void App::OnIdle(wxIdleEvent& event)
{
    if (g_sigtermReceived.exchange(false, std::memory_order_relaxed)) {
        m_sessionManagerShutdown = true;
        QuitAll();
    }
    event.Skip();
}

void App::RebuildWindowMenus()
{
    std::vector<MainFrame::WindowMenuEntry> entries;
    entries.reserve(m_windows.size());
    for (auto& w : m_windows) {
        MainFrame::WindowMenuEntry e;
        e.frame = w->frame;
        e.title = w->frame->GetTitle().ToStdString();
        if (w->uiManager)
            for (auto& [id, label] : w->uiManager->GetSessionList())
                e.sessions.push_back({id, label});
        entries.push_back(std::move(e));
    }
    for (auto& w : m_windows)
        w->frame->RebuildWindowMenu(entries);
}

int App::OnExit()
{
    m_saveTimer.Stop();
    ReleaseInstanceId(m_instanceId);
    libssh2_exit();
    return wxApp::OnExit();
}

void App::ApplyPreferences(const AppConfig& cfg)
{
    m_cfg = cfg;
    m_cfg.save(m_configPath);

    for (auto& wc : m_windows) {
        if (wc->uiManager) wc->uiManager->UpdateConfig(m_cfg);
        if (wc->frame)     wc->frame->UpdateConfig(m_cfg);
    }
}

bool App::HasRestoreState() const
{
    return m_restoreRepo && m_restoreRepo->HasSnapshot();
}

term::session::RestoreState App::BuildCurrentState() const
{
    term::session::RestoreState state;
    for (const auto& wc : m_windows) {
        if (!wc->uiManager) continue;
        term::session::RestoreWindow rw;
        const wxRect r = wc->frame->GetRect();
        rw.x      = r.x;
        rw.y      = r.y;
        rw.width  = r.width;
        rw.height = r.height;

        for (const auto& ts : wc->uiManager->GetTileSnapshots()) {
            term::session::RestoreTile rt;
            rt.activeTabIndex = ts.activeTabIndex;
            for (auto sid : ts.tabOrder) {
                term::session::Connection conn = m_sessionManager->GetConnection(sid);
                // Enrich PTY sessions with their live working directory.
                const std::string liveDir = m_sessionManager->GetCurrentWorkingDir(sid);
                if (!liveDir.empty())
                    conn.sessionInit.workingDir = liveDir;
                // Never persist passwords or passphrases.
                if (auto* ssh = std::get_if<term::session::SshDesc>(&conn.transport)) {
                    ssh->password   = {};
                    ssh->passphrase = {};
                }
                rt.sessions.push_back({std::move(conn)});
            }
            if (!rt.sessions.empty())
                rw.tiles.push_back(std::move(rt));
        }
        if (!rw.tiles.empty())
            state.windows.push_back(std::move(rw));
    }
    return state;
}

void App::SaveRestoreState()
{
    if (!m_restoreRepo) return;
    m_restoreRepo->Save(BuildCurrentState());
}

void App::RestoreStateImpl(const term::session::RestoreState& state, MainFrame* firstFrame)
{
    for (std::size_t wi = 0; wi < state.windows.size(); ++wi) {
        const auto& rw = state.windows[wi];

        MainFrame* frame = (wi == 0 && firstFrame) ? firstFrame : CreateNewWindow();
        frame->SetSize(rw.x, rw.y, rw.width, rw.height);

        for (const auto& rt : rw.tiles) {
            TerminalTile* currentTile = nullptr;
            for (std::size_t si = 0; si < rt.sessions.size(); ++si) {
                const auto& rs = rt.sessions[si];
                CreateSessionInTile(rs.conn, frame, currentTile);
                if (si == 0) {
                    // Record the newly created tile so subsequent sessions land in it.
                    WindowContext* ctx = FindContext(frame);
                    if (ctx)
                        currentTile = ctx->uiManager->GetActiveTile();
                }
            }
            // Restore active tab within the tile.
            if (currentTile && rt.activeTabIndex >= 0
                    && rt.activeTabIndex < static_cast<int>(rt.sessions.size())) {
                const term::session::SessionId activeId =
                    currentTile->GetSessionIdByTabIndex(rt.activeTabIndex);
                if (activeId) {
                    WindowContext* ctx = FindContext(frame);
                    if (ctx)
                        ctx->uiManager->RequestActivate(activeId);
                }
            }
        }
    }
    RebuildWindowMenus();
}

void App::RestoreWorkspaceFromMenu(MainFrame* callerFrame)
{
    if (!m_restoreRepo || !m_restoreRepo->HasSnapshot()) return;

    auto state = m_restoreRepo->Load();
    if (state.windows.empty()) return;

    // Reuse the calling frame if it is currently empty (no sessions).
    MainFrame* firstFrame = nullptr;
    if (callerFrame) {
        WindowContext* ctx = FindContext(callerFrame);
        if (ctx && !ctx->uiManager->HasAnySessions())
            firstFrame = callerFrame;
    }

    m_restoreRepo->Delete();  // Remove before restoring so restore state reflects new workspace.
    RestoreStateImpl(state, firstFrame);
}

bool App::HasNamedWorkspaces() const
{
    return m_namedRepo && !m_namedRepo->List().empty();
}

std::vector<std::string> App::GetNamedWorkspaceNames() const
{
    return m_namedRepo ? m_namedRepo->List() : std::vector<std::string>{};
}

bool App::HasNamedWorkspace(const std::string& name) const
{
    return m_namedRepo && m_namedRepo->Exists(name);
}

void App::SaveNamedWorkspace(const std::string& name)
{
    if (m_namedRepo)
        m_namedRepo->Save(name, BuildCurrentState());
}

void App::DeleteNamedWorkspace(const std::string& name)
{
    if (m_namedRepo)
        m_namedRepo->Delete(name);
}

void App::RestoreNamedWorkspace(const std::string& name, MainFrame* callerFrame)
{
    if (!m_namedRepo || !m_namedRepo->Exists(name)) return;

    auto state = m_namedRepo->Load(name);
    if (state.windows.empty()) return;

    // Reuse the calling frame if it currently has no sessions.
    MainFrame* firstFrame = nullptr;
    if (callerFrame) {
        WindowContext* ctx = FindContext(callerFrame);
        if (ctx && !ctx->uiManager->HasAnySessions())
            firstFrame = callerFrame;
    }

    // Named workspaces are persistent — NOT deleted after loading.
    RestoreStateImpl(state, firstFrame);
}

App::WindowContext* App::FindContext(MainFrame* frame)
{
    for (auto& w : m_windows)
        if (w->frame == frame) return w.get();
    return nullptr;
}

App::WindowContext* App::FindContextForSession(term::session::SessionId id)
{
    for (auto& w : m_windows)
        if (w->uiManager->HasSession(id)) return w.get();
    return nullptr;
}

bool App::DropSession(std::span<const term::session::SessionId> ids,
                      MainFrame*    dstFrame,
                      TerminalTile* dstTile)
{
    if (ids.empty()) return false;

    if (!dstFrame)
        dstFrame = CreateNewWindow();

    WindowContext* dstCtx = FindContext(dstFrame);
    if (!dstCtx) return false;

    TerminalTile* tile = dstTile;
    for (auto id : ids) {
        WindowContext* srcCtx = FindContextForSession(id);
        if (!srcCtx) continue;

        if (srcCtx != dstCtx) {
            m_sessionManager->SetSessionObserver(id, dstCtx->uiManager.get());
            m_sessionManager->ReassignRouter(id, *dstCtx->router);
        }
        // Release from source before taking on dest. For same-frame moves
        // (srcCtx == dstCtx) this prevents sessions_.emplace from silently
        // no-oping on an existing key. The drag path's post-hoc ReleaseSession
        // call becomes a safe no-op since FindSessionUI returns null.
        srcCtx->uiManager->ReleaseSession(id);
        dstCtx->uiManager->TakeSession(
            id,
            m_sessionManager->MakeTitleGetter(id),
            m_sessionManager->GetDocLayout(id).GetViewportCols(),
            m_sessionManager->GetDocLayout(id).GetViewportRows(),
            m_sessionManager->GetLabel(id),
            tile);
        if (!tile)
            tile = dstCtx->uiManager->GetActiveTile();
    }

    m_sessionManager->ActivateSession(ids.back(), *dstCtx->router);
    dstFrame->Raise();
    RebuildWindowMenus();
    return true;
}
