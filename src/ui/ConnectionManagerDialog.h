#pragma once

#include "config/Config.h"
#include "session/Connection.h"
#include <wx/dialog.h>
#include <functional>
#include <string>

class wxDataViewListCtrl;
class wxDataViewEvent;
class wxButton;

namespace term::db { class ConnectionStore; }

namespace ui {

class ConnectionManagerDialog : public wxDialog
{
public:
    using ConnectFn = std::function<void(const term::session::Connection&)>;

    ConnectionManagerDialog(wxWindow* parent,
                            term::db::ConnectionStore& store,
                            const AppConfig& cfg,
                            ConnectFn onConnect);

private:
    void PopulateList();
    void UpdateButtonState();
    std::string SelectedId() const;

    void OnConnect(wxCommandEvent&);
    void OnNew(wxCommandEvent&);
    void OnEdit(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnItemActivated(wxDataViewEvent&);
    void OnSelectionChanged(wxDataViewEvent&);

    // Quick-start handlers (hardcoded Defaults)
    void OnQuickLocalShell(wxCommandEvent&);
    void OnQuickLoopback(wxCommandEvent&);

    void LaunchProfile(const std::string& id);

    term::db::ConnectionStore& m_store;
    AppConfig                  m_cfg;
    ConnectFn                  m_onConnect;

    wxDataViewListCtrl* m_list     = nullptr;
    wxButton*           m_btnConn  = nullptr;
    wxButton*           m_btnEdit  = nullptr;
    wxButton*           m_btnDel   = nullptr;
};

} // namespace ui
