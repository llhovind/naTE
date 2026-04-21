#pragma once

#include <memory>
#include <functional>

#include "input/InputRouter.h"
#include "session/InputEncoder.h"
#include "transport/Transport.hpp"
#include "parser/Parser.h"

namespace term::session
{

    class Session : public input::InputTarget
    {
    public:
        using OutputCallback = std::function<void(const std::string &)>;

        explicit Session(std::unique_ptr<transport::Transport> transport);

        void OnInput(const input::KeyEvent &event) override;

        void SetOutputCallback(OutputCallback cb);

    private:
        void OnTransportData(const std::string &data);

    private:
        std::unique_ptr<transport::Transport> transport_;
        InputEncoder encoder_;
        parser::Parser parser_;

        OutputCallback output_;
    };

}
