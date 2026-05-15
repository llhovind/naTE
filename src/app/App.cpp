#include "app/App.h"
#include "db/JsonConnectionRepository.h"
#include "ui/MainFrame.h"
#include "ui/TerminalTile.h"
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <libssh2.h>
#include <cstdlib>
#include <string>
#include <algorithm>

bool App::OnInit() {
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
    wc->router = std::make_unique<term::input::InputRouter>();

    auto* frame = new MainFrame(m_cfg, *wc->router, *m_connectionStore);
    wc->frame = frame;

    wc->uiManager = std::make_unique<ui::UIManager>(
        *m_sessionManager, frame->GetConnMenu(), frame, m_cfg,
        *wc->router, frame->GetEditMenu());

    frame->SetUIManager(wc->uiManager.get());

    wc->uiManager->SetOnGridEmptyCallback([this, frame]() {
        if (m_windows.size() > 1)
            frame->CallAfter([frame]() { frame->Close(); });
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

    const term::session::SessionId id = m_sessionManager->CreateSession(
        conn,
        m_cfg.scrollbackLines,
        cols,
        static_cast<unsigned short>(m_cfg.rows),
        static_cast<unsigned short>(m_cfg.ptyLineWidth));

    m_sessionManager->SetSessionObserver(id, ctx->uiManager.get());
    m_sessionManager->RegisterRouter(id, *ctx->router);
    ctx->uiManager->TakeSession(
        id,
        m_sessionManager->MakeTitleGetter(id),
        cols,
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
    std::vector<std::pair<MainFrame*, std::string>> entries;
    entries.reserve(m_windows.size());
    for (auto& w : m_windows)
        entries.emplace_back(w->frame, w->frame->GetTitle().ToStdString());
    for (auto& w : m_windows)
        w->frame->RebuildWindowMenu(entries);
}

int App::OnExit()
{
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
