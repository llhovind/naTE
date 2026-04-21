#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include <wx/menu.h>
#include <wx/sizer.h>

MainFrame::MainFrame(const AppConfig &cfg)
    : wxFrame(nullptr, wxID_ANY, "naTE")
{
    auto *menu = new wxMenu;
    menu->Append(wxID_EXIT, "Quit\tCtrl+Q");
    Bind(wxEVT_MENU, &MainFrame::OnQuit, this, wxID_EXIT);

    auto *menuBar = new wxMenuBar;
    menuBar->Append(menu, "&File");
    SetMenuBar(menuBar);

    CreateStatusBar();
    SetStatusText("Ready");

    m_panel = new TerminalPanel(this, cfg);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_panel, 1, wxEXPAND);
    SetSizerAndFit(sizer);
}

void MainFrame::OnQuit(wxCommandEvent &)
{
    Close(true);
}