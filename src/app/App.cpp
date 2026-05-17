#include "app/App.h"
#include "db/JsonConnectionRepository.h"
#include "ui/MainFrame.h"
#include "ui/TerminalTile.h"
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <libssh2.h>
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
    libssh2_init(0);

    const wxString exeDir =
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    m_cfg = AppConfig::load((exeDir + wxFileName::GetPathSeparator() + "config.ini").ToStdString());

    const std::string connectionsPath = [] {
        const char* home = std::getenv("HOME");
        const std::string base = home ? std::string(home) + "/.nate" : std::string(".nate");
        return base + "/connections.json";
    }();
    m_connectionStore = std::make_unique<term::db::ConnectionStore>(
        std::make_unique<term::db::JsonConnectionRepository>(connectionsPath));

    m_sessionManager = std::make_unique<term::session::SessionManager>();

    CreateNewWindow();
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

    term::session::SessionId id = 0;
    try {
        id = m_sessionManager->CreateSession(
            conn,
            m_cfg.scrollbackLines,
            cols,
            rows,
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
        m_sessionManager->MakeTitleGetter(id),
        cols,
        rows,
        conn.label,
        targetTile);
    return id;
}


void App::QuitAll()
{
    std::vector<MainFrame*> frames;
    frames.reserve(m_windows.size());
    for (auto& w : m_windows)
        frames.push_back(w->frame);
    for (auto* f : frames)
        f->Close(true);
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
    ReleaseInstanceId(m_instanceId);
    libssh2_exit();
    return wxApp::OnExit();
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
