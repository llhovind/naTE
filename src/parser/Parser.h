#pragma once

#include <functional>
#include <string>

namespace term::parser
{

    class Parser
    {
    public:
        using OutputCallback = std::function<void(const std::string &)>;

        explicit Parser(OutputCallback cb);

        void Process(const std::string &data);

    private:
        OutputCallback callback_;
    };

}
