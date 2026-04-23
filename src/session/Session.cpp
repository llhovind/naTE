#include "session/Session.h"

namespace term::session {

Session::Session(std::unique_ptr<transport::Transport> transport, int scrollbackLines)
    : transport_(std::move(transport)),
      parser_(*this),
      main_doc_(std::make_unique<MainScreenDocument>(scrollbackLines)),
      alt_doc_(std::make_unique<AltScreenDocument>()),
      active_doc_(main_doc_.get()),
      docLayout_(std::make_unique<DocLayout>(*main_doc_))
{
    transport_->SetReadCallback(
        [this](const std::string& data) { OnTransportData(data); });
}

void Session::OnInput(const input::KeyEvent& event)
{
    auto bytes = encoder_.Encode(event);
    if (!bytes.empty())
        transport_->Write(bytes);
}

void Session::OnAppendInsertChar(char32_t ch)
{
    active_doc_->AppendInsertChar(ch);
}

void Session::OnBackspace()
{
    active_doc_->Backspace();
}

void Session::OnNewLine()
{
    active_doc_->NewLine();
}

void Session::OnSetStyle(const Style& style)
{
    active_doc_->SetCurrentStyle(style);
}

void Session::SetRefreshCallback(RefreshCallback cb)
{
    refresh_callback_ = std::move(cb);
}

void Session::SetTitleCallback(TitleCallback cb)
{
    title_callback_ = std::move(cb);
}

void Session::SetDisconnectCallback(DisconnectCallback cb)
{
    disconnect_callback_ = std::move(cb);
    transport_->SetDisconnectCallback(disconnect_callback_);
}

void Session::OnSetTitle(const std::string& title)
{
    title_ = title;
    if (title_callback_)
        title_callback_(title_);
}

DocLayout& Session::GetDocLayout()
{
    return *docLayout_;
}

void Session::OnTransportData(const std::string& data)
{
    parser_.Process(data);
    if (refresh_callback_)
        refresh_callback_();
}

} // namespace term::session
