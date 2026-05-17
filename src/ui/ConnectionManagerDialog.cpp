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
#include <wx/statline.h>
#include <wx/stattext.h>

#include <algorithm>
#include <ctime>

namespace ui {

namespace {

constexpr int ID_BTN_CONNECT     = wxID_HIGHEST + 300;
constexpr int ID_BTN_NEW         = wxID_HIGHEST + 301;
constexpr int ID_BTN_EDIT        = wxID_HIGHEST + 302;
constexpr int ID_BTN_DELETE      = wxID_HIGHEST + 303;
constexpr int ID_BTN_QUICK_SHELL = wxID_HIGHEST + 304;
constexpr int ID_BTN_QUICK_LB    = wxID_HIGHEST + 305;

std::string TransportTypeName(const term::session::TransportDesc& t)
{
    return std::visit([](const auto& d) -> std::string {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, term::session::SshDesc>)       return "SSH";
        if constexpr (std::is_same_v<T, term::session::PtyDesc>)       return "PTY";
        if constexpr (std::is_same_v<T, term::session::LoopbackDesc>)  return "Loopback";
        if constexpr (std::is_same_v<T, term::session::SerialDesc>)    return "Serial";
        return "Unknown";
    }, t);
}

std::string TransportInfo(const term::session::TransportDesc& t)
{
    return std::visit([](const auto& d) -> std::string {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, term::session::SshDesc>)
            return d.username + "@" + d.host + ":" + std::to_string(d.port);
        if constexpr (std::is_same_v<T, term::session::PtyDesc>)
            return d.shell;
        if constexpr (std::is_same_v<T, term::session::SerialDesc>)
            return d.device + " " + std::to_string(d.baudRate) + "bps";
        return {};
    }, t);
}

std::string FormatLastUsed(std::time_t t)
{
    if (t == 0) return "Never";
    const std::time_t now = std::time(nullptr);
    const double secs = std::difftime(now, t);
    if (secs < 60)         return "Just now";
    if (secs < 3600)       return std::to_string(static_cast<int>(secs / 60)) + "m ago";
    if (secs < 86400)      return std::to_string(static_cast<int>(secs / 3600)) + "h ago";
    if (secs < 86400 * 7)  return std::to_string(static_cast<int>(secs / 86400)) + "d ago";
    char buf[32];
    struct tm tm_info{};
    localtime_r(&t, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_info);
    return buf;
}


} // namespace

// ---- ConnectionManagerDialog -----------------------------------------------

ConnectionManagerDialog::ConnectionManagerDialog(wxWindow* parent,
                                                  term::db::ConnectionStore& store,
                                                  const AppConfig& cfg,
                                                  ConnectFn onConnect)
    : wxDialog(parent, wxID_ANY, "Connection Manager",
               wxDefaultPosition, wxSize(600, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_store(store)
    , m_cfg(cfg)
    , m_onConnect(std::move(onConnect))
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ---- Quick start --------------------------------------------------------
    outer->Add(new wxStaticText(this, wxID_ANY, "Quick start:"), 0,
               wxLEFT | wxTOP, 12);

    auto* quickRow = new wxBoxSizer(wxHORIZONTAL);
    quickRow->Add(new wxButton(this, ID_BTN_QUICK_SHELL, "Local Shell"), 0, wxRIGHT, 6);
    quickRow->Add(new wxButton(this, ID_BTN_QUICK_LB,   "Loopback"),    0);
    outer->Add(quickRow, 0, wxLEFT | wxTOP | wxBOTTOM, 12);

    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->Add(new wxStaticText(this, wxID_ANY, "Saved connections:"), 0,
               wxLEFT | wxTOP, 12);

    // ---- Saved connections list ---------------------------------------------
    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxDV_ROW_LINES | wxDV_SINGLE);
    m_list->AppendTextColumn("Name",      wxDATAVIEW_CELL_INERT, 160, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Type",      wxDATAVIEW_CELL_INERT,  70, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Host/Info", wxDATAVIEW_CELL_INERT, 200, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn("Last Used", wxDATAVIEW_CELL_INERT, 100, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    outer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Action buttons -----------------------------------------------------
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    m_btnConn = new wxButton(this, ID_BTN_CONNECT, "Connect");
    m_btnEdit = new wxButton(this, ID_BTN_EDIT,    "Edit...");
    m_btnDel  = new wxButton(this, ID_BTN_DELETE,  "Delete");
    btnRow->Add(m_btnConn, 0, wxRIGHT, 6);
    btnRow->Add(new wxButton(this, ID_BTN_NEW, "New..."), 0, wxRIGHT, 6);
    btnRow->Add(m_btnEdit,  0, wxRIGHT, 6);
    btnRow->Add(m_btnDel,   0, wxRIGHT, 20);
    m_cbOpenNewWindow = new wxCheckBox(this, wxID_ANY, "Open in New Window");
    btnRow->Add(m_cbOpenNewWindow, 0, wxALIGN_CENTER_VERTICAL);
    btnRow->AddStretchSpacer();
    btnRow->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
    outer->Add(btnRow, 0, wxEXPAND | wxALL, 12);

    SetSizer(outer);

    PopulateList();
    UpdateButtonState();

    // Events
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnConnect,        this, ID_BTN_CONNECT);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnNew,            this, ID_BTN_NEW);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnEdit,           this, ID_BTN_EDIT);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnDelete,         this, ID_BTN_DELETE);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnQuickLocalShell,this, ID_BTN_QUICK_SHELL);
    Bind(wxEVT_BUTTON, &ConnectionManagerDialog::OnQuickLoopback,  this, ID_BTN_QUICK_LB);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&){ EndModal(wxID_CLOSE); }, wxID_CLOSE);

    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                 &ConnectionManagerDialog::OnItemActivated, this);
    m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
                 &ConnectionManagerDialog::OnSelectionChanged, this);
}

void ConnectionManagerDialog::PopulateList()
{
    m_list->DeleteAllItems();
    for (const auto& p : m_store.GetAll()) {
        wxVector<wxVariant> row;
        row.push_back(p.name);
        row.push_back(TransportTypeName(p.transport));
        row.push_back(TransportInfo(p.transport));
        row.push_back(FormatLastUsed(p.lastUsed));
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
    conn.label       = it->name;
    conn.transport   = it->transport;
    conn.wrapMode    = it->wrapMode;
    conn.columnWidth = it->columnWidth;
    conn.rows        = it->rows;

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
    const std::string defaultShell = [] {
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/bash");
    }();

    NewConnectionDialog dlg(this, defaultShell, m_cfg.geometryPresets,
                            {}, LaunchContext::ProfileOnly);
    if (dlg.ShowModal() != wxID_OK) return;

    const term::session::Connection conn = ui::ToConnection(dlg.GetParams());
    const std::string dlgName = dlg.GetProfileName();
    m_store.Add(dlgName.empty() ? conn.label : dlgName,
                conn.transport, conn.wrapMode, conn.columnWidth, conn.rows);
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

    const std::string defaultShell = [] {
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/bash");
    }();

    NewConnectionDialog dlg(this, defaultShell, m_cfg.geometryPresets,
                            {}, LaunchContext::ProfileOnly, &*it);
    if (dlg.ShowModal() != wxID_OK) return;

    term::db::ConnectionProfile updated = *it;
    const std::string dlgName = dlg.GetProfileName();
    if (!dlgName.empty())
        updated.name = dlgName;
    {
        const term::session::Connection conn = ui::ToConnection(dlg.GetParams());
        updated.transport   = conn.transport;
        updated.wrapMode    = conn.wrapMode;
        updated.columnWidth = conn.columnWidth;
        updated.rows        = conn.rows;
    }

    m_store.Update(updated);
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

void ConnectionManagerDialog::OnQuickLocalShell(wxCommandEvent&)
{
    const char* s = std::getenv("SHELL");
    const std::string shell = s ? s : "/bin/bash";

    term::session::Connection conn;
    conn.label     = "Local Shell";
    conn.transport = term::session::PtyDesc{shell};
    conn.wrapMode  = false;
    conn.columnWidth = m_cfg.geometryPresets.empty() ? 80 : m_cfg.geometryPresets[0].cols;

    m_onConnect(conn, m_cbOpenNewWindow->GetValue());
    EndModal(wxID_OK);
}

void ConnectionManagerDialog::OnQuickLoopback(wxCommandEvent&)
{
    term::session::Connection conn;
    conn.label     = "Loopback";
    conn.transport = term::session::LoopbackDesc{};
    conn.wrapMode  = true;
    conn.columnWidth = m_cfg.geometryPresets.empty() ? 80 : m_cfg.geometryPresets[0].cols;

    m_onConnect(conn, m_cbOpenNewWindow->GetValue());
    EndModal(wxID_OK);
}

} // namespace ui
