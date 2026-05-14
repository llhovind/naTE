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

    wc->uiManager->SetMoveSessionCallback(
        [this, frame](term::session::SessionId id, MainFrame* dstFrame) {
            MoveSession(frame, id, dstFrame, nullptr);
        });

    wc->uiManager->SetMoveTabToTileCallback(
        [this, frame](term::session::SessionId id, MainFrame* dstFrame,
                      TerminalTile* dstTile) {
            MoveSession(frame, id, dstFrame, dstTile);
        });

    wc->uiManager->SetMoveTileCallback(
        [this, frame](std::vector<term::session::SessionId> ids, MainFrame* dstFrame) {
            WindowContext* srcCtx = FindContext(frame);
            WindowContext* dstCtx = FindContext(dstFrame);
            if (!srcCtx || !dstCtx || srcCtx == dstCtx || ids.empty()) return;

            // Move all sessions in tab order into one new tile at the destination.
            TerminalTile* newTile = nullptr;
            for (auto id : ids) {
                const unsigned short cols =
                    m_sessionManager->GetDocLayout(id).GetViewportCols();
                const std::string label = m_sessionManager->GetLabel(id);

                srcCtx->uiManager->ReleaseSession(id);
                m_sessionManager->SetSessionObserver(id, dstCtx->uiManager.get());
                m_sessionManager->ReassignRouter(id, *srcCtx->router, *dstCtx->router);
                dstCtx->uiManager->TakeSession(
                    id, m_sessionManager->MakeTitleGetter(id), cols, label, newTile);
                if (!newTile)
                    newTile = dstCtx->uiManager->GetActiveTile();
            }
            m_sessionManager->ActivateSession(ids.back(), *dstCtx->router);
            dstFrame->Raise();
            RebuildWindowMenus();
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

void App::MoveSession(MainFrame* src, term::session::SessionId id, MainFrame* dst,
                      TerminalTile* dstTile)
{
    WindowContext* srcCtx = FindContext(src);
    WindowContext* dstCtx = FindContext(dst);
    if (!srcCtx || !dstCtx || srcCtx == dstCtx) return;

    srcCtx->uiManager->ReleaseSession(id);

    m_sessionManager->SetSessionObserver(id, dstCtx->uiManager.get());
    m_sessionManager->ReassignRouter(id, *srcCtx->router, *dstCtx->router);

    dstCtx->uiManager->TakeSession(
        id,
        m_sessionManager->MakeTitleGetter(id),
        m_sessionManager->GetDocLayout(id).GetViewportCols(),
        m_sessionManager->GetLabel(id),
        dstTile);

    m_sessionManager->ActivateSession(id, *dstCtx->router);
    dst->Raise();
    RebuildWindowMenus();
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
