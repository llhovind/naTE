#include "Session.h"

namespace term::session
{

    Session::Session(std::unique_ptr<transport::Transport> transport)
        : transport_(std::move(transport)),
          parser_([this](const std::string &data)
                  {
          if (output_) output_(data); })
    {
        transport_->SetReadCallback(
            [this](const std::string &data)
            {
                OnTransportData(data);
            });
    }

    void Session::OnInput(const input::KeyEvent &event)
    {
        auto bytes = encoder_.Encode(event);
        if (!bytes.empty())
        {
            transport_->Write(bytes);
        }
    }

    void Session::OnTransportData(const std::string &data)
    {
        parser_.Process(data);
    }

    void Session::SetOutputCallback(OutputCallback cb)
    {
        output_ = std::move(cb);
    }

}
