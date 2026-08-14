#include "ui/FileExplorerManager.h"

#include "ui/DialogPlacement.h"

#include <wx/display.h>
#include <wx/msgdlg.h>
#include <wx/window.h>

namespace ui {

namespace {

// How far each additional window is stepped down and right from the saved
// position. Roughly a title bar: far enough that the window beneath is visibly
// there and can be clicked, close enough that the group still reads as one.
constexpr int kCascadeStep = 28;

// How much of a restored window must land on a display for the position to be
// used. A title bar's worth in each direction — below that there is nothing
// left to drag the window back by.
constexpr int kMinOnScreen = 80;

std::vector<ScreenRect> DisplayRects()
{
    std::vector<ScreenRect> rects;
    const unsigned count = wxDisplay::GetCount();
    rects.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        // The client area, not the full geometry: a window placed under a panel
        // or a dock is as hard to reach as one placed off the screen entirely.
        const wxRect r = wxDisplay(i).GetClientArea();
        rects.push_back(ScreenRect{r.x, r.y, r.width, r.height});
    }
    return rects;
}

} // namespace

FileExplorerManager::FileExplorerManager(
    term::session::SessionManager& sm,
    const AppConfig& cfg,
    std::function<void(term::session::SessionId, std::string)> onOpenInEditor,
    std::function<void(const FileExplorerLayout&)> onLayoutChanged)
    : sm_(sm)
    , cfg_(cfg)
    , onOpenInEditor_(std::move(onOpenInEditor))
    , onLayoutChanged_(std::move(onLayoutChanged))
{}

FileExplorerManager::~FileExplorerManager()
{
    // Clear every callback back into this object before it can fire: the
    // frames are owned by wx and may outlive this manager during application
    // teardown, and either callback would then touch a destroyed manager.
    for (auto& [id, frame] : frames_) {
        if (!frame) continue;
        frame->SetOnClosed(nullptr);
        frame->SetOnLayoutChanged(nullptr);
        frame->SetOnOpenInEditor(nullptr);
    }
}

void FileExplorerManager::OpenForSession(wxWindow* parent,
                                         term::session::SessionId id,
                                         FileExplorerMode mode)
{
    if (!id) return;

    if (const auto it = frames_.find(id); it != frames_.end() && it->second) {
        it->second->SetMode(mode);
        it->second->Raise();
        it->second->SetFocus();
        return;
    }

    if (!sm_.SupportsFileTransfer(id)) {
        wxMessageBox("This session has no remote filesystem.\n\n"
                     "The file explorer is available for SSH sessions.",
                     "File Explorer", wxOK | wxICON_INFORMATION, parent);
        return;
    }

    // The endpoint comes from the pane that raised it, not from the session
    // this window was opened for: either pane may be showing another session
    // or this computer, and routing a path to the wrong machine silently edits
    // the wrong file when both happen to have it.
    auto* frame = new FileExplorerFrame(
        parent, id, sm_, cfg_,
        [this](term::session::SessionId endpoint, std::string path) {
            if (onOpenInEditor_) onOpenInEditor_(endpoint, std::move(path));
        });

    frame->SetOnClosed([this, id] { frames_.erase(id); });

    // Mode before the layout callback: the PersistLayout inside ApplyMode then
    // no-ops, so merely opening a window never rewrites config.ini. It is also
    // before placement, because Transfer mode is twice as wide and the position
    // has to be judged against the size the window will actually have.
    frame->SetMode(mode);
    PlaceFrame(*frame);

    frame->SetOnLayoutChanged(
        [this](const FileExplorerLayout& layout) {
            // Mirror it locally too, so a window opened later this session
            // starts the same shape without waiting for a config reload.
            cfg_.fileExplorerWidth        = layout.width;
            cfg_.fileExplorerHeight       = layout.height;
            cfg_.fileExplorerX            = layout.x;
            cfg_.fileExplorerY            = layout.y;
            cfg_.fileExplorerColumnWidths = layout.columnWidths;
            if (onLayoutChanged_) onLayoutChanged_(layout);
        });
    frames_[id] = frame;
    frame->Show();
}

void FileExplorerManager::PlaceFrame(FileExplorerFrame& frame) const
{
    // Never placed: leave the window manager to decide, which it does better
    // than any coordinate this application could invent for a first run.
    if (cfg_.fileExplorerX == kUnsetWindowCoord ||
        cfg_.fileExplorerY == kUnsetWindowCoord)
        return;

    // Windows already open are stepped past rather than stacked on. Replaying
    // one saved position for every window would drop each new explorer exactly
    // onto the last, which reads as the second one having failed to open.
    const int offset = static_cast<int>(frames_.size()) * kCascadeStep;
    const wxSize size = frame.GetSize();
    const ScreenRect wanted{cfg_.fileExplorerX + offset,
                            cfg_.fileExplorerY + offset,
                            size.x, size.y};

    // The display that held this window may be gone — a dock undocked, a second
    // monitor unplugged — and replaying the coordinates would then open it
    // somewhere the user cannot see or reach it.
    const auto displays = DisplayRects();
    if (!RectIsReachable(wanted, displays.data(), displays.size(), kMinOnScreen))
        return;

    frame.PlaceAt(wxPoint(wanted.x, wanted.y));
}

void FileExplorerManager::OnSessionDestroyed(term::session::SessionId id)
{
    // Broadcast rather than look up: a window opened for one session may have
    // either of its panes pointed at another, so every frame has to decide for
    // itself whether this one mattered to it.
    for (auto& [openedFor, frame] : frames_)
        if (frame) frame->OnSessionEnded(id);
}

void FileExplorerManager::OnFileChanged(term::session::SessionId endpoint,
                                        const std::string& path)
{
    // Broadcast for the same reason as OnSessionDestroyed: any window's pane
    // may be pointed at this endpoint, not just the one opened for it.
    for (auto& [openedFor, frame] : frames_)
        if (frame) frame->OnFileChanged(endpoint, path);
}

void FileExplorerManager::UpdateConfig(const AppConfig& cfg)
{
    cfg_ = cfg;
    for (auto& [id, frame] : frames_)
        if (frame) frame->ApplyConfig(cfg_);
}

} // namespace ui
