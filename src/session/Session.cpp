#include "session/Session.h"

namespace term::session {

Session::Session(std::unique_ptr<transport::Transport> transport)
    : transport_(std::move(transport)),
      parser_(*this),
      main_doc_(std::make_unique<MainScreenDocument>()),
      alt_doc_(std::make_unique<AltScreenDocument>()),
      active_doc_(main_doc_.get()),
      layout_(std::make_unique<Layout>(*main_doc_, 80))
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

void Session::OnAppendChar(char32_t ch)
{
    active_doc_->AppendChar(ch);
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

Layout& Session::GetLayout()
{
    return *layout_;
}

void Session::OnTransportData(const std::string& data)
{
    parser_.Process(data);
    if (refresh_callback_)
        refresh_callback_();
}

} // namespace term::session
