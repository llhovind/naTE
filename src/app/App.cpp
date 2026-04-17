#include "app/App.h"
#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include <wx/filename.h>
#include <wx/stdpaths.h>

bool App::OnInit() {
    const wxString exeDir =
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    m_cfg = AppConfig::load(exeDir + wxFileName::GetPathSeparator() + "config.ini");

    auto* frame = new MainFrame(m_cfg);
    m_panels.push_back(frame->GetPanel());
    frame->Show();
    return true;
}
