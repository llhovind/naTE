#include "Parser.h"

namespace term::parser
{

    Parser::Parser(OutputCallback cb)
        : callback_(std::move(cb)) {}

    void Parser::Process(const std::string &data)
    {
        if (callback_)
        {
            callback_(data); // pass-through
        }
    }

}
