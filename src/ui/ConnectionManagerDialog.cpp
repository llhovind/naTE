#include "ui/ConnectionManagerDialog.h"
#include "ui/ConnectionFactory.h"
#include "db/ConnectionStore.h"
#include "db/ConnectionProfile.h"
#include "session/Connection.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dataview.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>

namespace ui {

namespace {

constexpr int ID_BTN_CONNECT = wxID_HIGHEST + 300;
constexpr int ID_BTN_NEW     = wxID_HIGHEST + 301;
constexpr int ID_BTN_EDIT    = wxID_HIGHEST + 302;
constexpr int ID_BTN_DELETE  = wxID_HIGHEST + 303;

std::string TransportTypeName(const term::transport::TransportDesc& t)
{
    return std::visit([](const auto& d) -> std::string {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, term::transport::SshDesc>)       return "SSH";
        if constexpr (std::is_same_v<T, term::transport::PtyDesc>)       return "PTY";
        if constexpr (std::is_same_v<T, term::transport::LoopbackDesc>)  return "Loopback";
        if constexpr (std::is_same_v<T, term::transport::SerialDesc>)    return "Serial";
        return "Unknown";
    }, t);
}

std::string TransportHost(const term::transport::TransportDesc& t)
{
    return std::visit([](const auto& d) -> std::string {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, term::transport::SshDesc>) {
            std::string h = d.username + "@" + d.host;
            if (d.port != 22) h += ":" + std::to_string(d.port);
            return h;
        }
        if constexpr (std::is_same_v<T, term::transport::SerialDesc>)
            return d.device;
        return "Local";
    }, t);
}

std::string TransportFlags(const term::transport::TransportDesc& t)
{
    return std::visit([](const auto& d) -> std::string {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, term::transport::SshDesc>) {
            std::string f;
            if (d.x11Forwarding)           f += "X11 ";
            if (d.agentForwarding)         f += "Agent ";
            if (d.proxyJump.has_value() && !d.proxyJump->host.empty()) f += "Jump";
            if (!f.empty() && f.back() == ' ') f.pop_back();
            return f.empty() ? "-" : f;
        }
        return "-";
    }, t);
}

} // namespace

// ---- ConnectionManagerDialog -----------------------------------------------

ConnectionManagerDialog::ConnectionManagerDialog(wxWindow* parent,
                                                  term::db::ConnectionStore& store,
                                                  const AppConfig& cfg,
                                                  ConnectFn onConnect)
    : wxDialog(parent, wxID_ANY, "Connection Manager",
               wxDefaultPosition, wxSize(840, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_store(store)
    , m_cfg(cfg)
    , m_onConnect(std::move(onConnect))
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ---- Top action bar -----------------------------------------------------
    auto* topRow = new wxBoxSizer(wxHORIZONTAL);
    topRow->Add(new wxStaticText(this, wxID_ANY, "Saved connections:"),
                0, wxALIGN_CENTER_VERTICAL);
    topRow->AddStretchSpacer();
    m_cbOpenNewWindow = new wxCheckBox(this, wxID_ANY, "Open in New Window");
    topRow->Add(m_cbOpenNewWindow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    m_btnConn = new wxButton(this, ID_BTN_CONNECT, "Connect");
    topRow->Add(m_btnConn, 0);
    outer->Add(topRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Saved connections list ---------------------------------------------
    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxDV_ROW_LINES | wxDV_SINGLE);
    m_list->AppendTextColumn("Name",      wxDATAVIEW_CELL_INERT, 150, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Transport", wxDATAVIEW_CELL_INERT,  70, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Host",      wxDATAVIEW_CELL_INERT, 150, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Flags",     wxDATAVIEW_CELL_INERT, 100, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("CWD",       wxDATAVIEW_CELL_INERT, 110, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Geometry",  wxDATAVIEW_CELL_INERT,  65, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Wrap",      wxDATAVIEW_CELL_INERT,  50, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    outer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Action buttons -----------------------------------------------------
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    m_btnEdit = new wxButton(this, ID_BTN_EDIT,   "Edit...");
    m_btnDel  = new wxButton(this, ID_BTN_DELETE, "Delete");
    btnRow->Add(new wxButton(this, ID_BTN_NEW, "New..."), 0, wxRIGHT, 6);
    btnRow->Add(m_btnEdit, 0, wxRIGHT, 6);
    btnRow->Add(m_btnDel,  0);
    btnRow->AddStretchSpacer();
    btnRow->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
    outer->Add(btnRow, 0, wxEXPAND | wxALL, 12);

    SetSizer(outer);

    PopulateList();
    UpdateButtonState();

    // Events
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnConnect, this, ID_BTN_CONNECT);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnNew,     this, ID_BTN_NEW);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnEdit,    this, ID_BTN_EDIT);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnDelete,  this, ID_BTN_DELETE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&){ EndModal(wxID_CLOSE); }, wxID_CLOSE);

    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                 &ConnectionManagerDialog::OnItemActivated, this);
    m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
                 &ConnectionManagerDialog::OnSelectionChanged, this);
}

void ConnectionManagerDialog::PopulateList()
{
    for (unsigned r = 0; r < m_list->GetItemCount(); ++r)
        delete reinterpret_cast<std::string*>(m_list->GetItemData(m_list->RowToItem(r)));
    m_list->DeleteAllItems();
    for (const auto& p : m_store.GetAll()) {
        wxVector<wxVariant> row;
        row.push_back(p.name);
        row.push_back(TransportTypeName(p.transport));
        row.push_back(TransportHost(p.transport));
        row.push_back(TransportFlags(p.transport));
        row.push_back(p.sessionInit.workingDir.empty() ? "-" : p.sessionInit.workingDir);
        row.push_back(std::to_string(p.columnWidth) + "x" + std::to_string(p.rows));
        row.push_back(p.wrapMode ? "On" : "Off");
        m_list->AppendItem(row, reinterpret_cast<wxUIntPtr>(new std::string(p.id)));
    }
}

void ConnectionManagerDialog::UpdateButtonState()
{
    const bool hasSel = m_list->GetSelectedRow() != wxNOT_FOUND;
    m_btnConn->Enable(hasSel);
    m_btnEdit->Enable(hasSel);
    m_btnDel->Enable(hasSel);
}

std::string ConnectionManagerDialog::SelectedId() const
{
    const int row = m_list->GetSelectedRow();
    if (row == wxNOT_FOUND) return {};
    auto* idPtr = reinterpret_cast<std::string*>(
        m_list->GetItemData(m_list->RowToItem(row)));
    return idPtr ? *idPtr : std::string{};
}

void ConnectionManagerDialog::LaunchProfile(const std::string& id)
{
    const auto& profiles = m_store.GetAll();
    auto it = std::find_if(profiles.begin(), profiles.end(),
                           [&](const term::db::ConnectionProfile& p){ return p.id == id; });
    if (it == profiles.end()) return;

    m_store.UpdateLastUsed(id);

    term::session::Connection conn;
    conn.label           = it->name;
    conn.transport       = it->transport;
    conn.wrapMode        = it->wrapMode;
    conn.columnWidth     = it->columnWidth;
    conn.rows            = it->rows;
    conn.sessionInit     = it->sessionInit;
    conn.profileTitle    = it->profileTitle;
    conn.useProfileTitle = it->useProfileTitle;

    m_onConnect(conn, m_cbOpenNewWindow->GetValue());
    EndModal(wxID_OK);
}

void ConnectionManagerDialog::OnConnect(wxCommandEvent&)
{
    LaunchProfile(SelectedId());
}

void ConnectionManagerDialog::OnItemActivated(wxDataViewEvent&)
{
    LaunchProfile(SelectedId());
}

void ConnectionManagerDialog::OnSelectionChanged(wxDataViewEvent&)
{
    UpdateButtonState();
}

void ConnectionManagerDialog::OnNew(wxCommandEvent&)
{
    const std::string defaultShell = [&] {
        if (!m_cfg.defaultShell.empty()) return m_cfg.defaultShell;
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/sh");
    }();

    NewConnectionDialog dlg(this, defaultShell, m_cfg.defaultWorkingDir,
                            m_cfg.defaultWrapMode,
                            m_cfg.defaultLoginShell,
                            m_cfg.geometryPresets, {}, LaunchContext::ProfileOnly);
    if (dlg.ShowModal() != wxID_OK) return;

    const term::session::Connection conn = ui::ToConnection(dlg.GetParams());
    const std::string name = dlg.GetProfileName();
    ui::SaveProfile(m_store, conn, name.empty() ? conn.label : name);
    PopulateList();
    UpdateButtonState();
}

void ConnectionManagerDialog::OnEdit(wxCommandEvent&)
{
    const std::string id = SelectedId();
    if (id.empty()) return;

    const auto& profiles = m_store.GetAll();
    auto it = std::find_if(profiles.begin(), profiles.end(),
                           [&](const term::db::ConnectionProfile& p){ return p.id == id; });
    if (it == profiles.end()) return;

    const std::string defaultShell = [&] {
        if (!m_cfg.defaultShell.empty()) return m_cfg.defaultShell;
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/sh");
    }();

    NewConnectionDialog dlg(this, defaultShell, m_cfg.defaultWorkingDir,
                            m_cfg.defaultWrapMode,
                            m_cfg.defaultLoginShell,
                            m_cfg.geometryPresets, {}, LaunchContext::ProfileOnly, &*it);
    if (dlg.ShowModal() != wxID_OK) return;

    const term::session::Connection conn = ui::ToConnection(dlg.GetParams());
    const std::string name = dlg.GetProfileName();
    ui::SaveProfile(m_store, conn, name.empty() ? it->name : name, id);
    PopulateList();
    UpdateButtonState();
}

void ConnectionManagerDialog::OnDelete(wxCommandEvent&)
{
    const std::string id = SelectedId();
    if (id.empty()) return;

    const auto& profiles = m_store.GetAll();
    auto it = std::find_if(profiles.begin(), profiles.end(),
                           [&](const term::db::ConnectionProfile& p){ return p.id == id; });
    const std::string name = (it != profiles.end()) ? it->name : id;

    const int answer = wxMessageBox(
        wxString::Format("Delete \"%s\"?", name),
        "Confirm Delete", wxYES_NO | wxICON_WARNING, this);
    if (answer != wxYES) return;

    m_store.Remove(id);
    PopulateList();
    UpdateButtonState();
}

} // namespace ui
