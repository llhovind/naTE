#include "session/Session.h"

namespace term::session {

Session::Session(std::unique_ptr<transport::Transport> transport)
    : transport_(std::move(transport)),
      parser_(*this),
      main_doc_(std::make_unique<MainScreenDocument>()),
      alt_doc_(std::make_unique<AltScreenDocument>()),
      active_doc_(main_doc_.get()),
      docLayout_(std::make_unique<DocLayout>(*main_doc_, 80))
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
