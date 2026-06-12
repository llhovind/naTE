#pragma once

#include "document/Document.h"
#include "parser/IScreenTarget.h"
#include <string>

namespace term::parser {

class Parser {
public:
    Parser(IDocumentTarget& docTarget, IScreenTarget& screenTarget);

    void Process(const std::string& data);
    void Reset();

    // SGR state is terminal-global, not per-document: re-apply the accumulated
    // style so output on the newly active document (e.g. after an alt-screen
    // switch) continues with the attributes set while the previous one was active.
    void SetDocTarget(IDocumentTarget* target)
    {
        doc_ = target;
        doc_->SetCurrentStyle(currentStyle_);
    }

private:
    enum class State { Normal, Escape, Ss3, Csi, Osc, OscEsc, SkipOne };

    void HandleNormal(unsigned char byte);
    void HandleEscape(unsigned char byte);
    void HandleSs3(unsigned char byte);
    void HandleCsi(unsigned char byte);
    void HandleCsiFinal(unsigned char byte);
    void HandleCsiPrivate(unsigned char byte);
    void HandleOsc(unsigned char byte);
    void HandleOscEsc(unsigned char byte);
    void HandleSkipOne(unsigned char byte);

    void DispatchSgr();
    void DispatchOsc();
    int  ParseFirstParam(int defaultVal) const;
    int  ParseParam(int index, int defaultVal) const;

    IDocumentTarget* doc_;
    IScreenTarget&   screen_;
    State            state_        = State::Normal;
    std::string      params_;
    bool             privateMode_  = false;
    std::string      osc_payload_;
    Style            currentStyle_;  // accumulated SGR state; mutated in place by DispatchSgr

    char32_t utf8_codepoint_ = 0;
    int      utf8_remaining_ = 0;
};

} // namespace term::parser
