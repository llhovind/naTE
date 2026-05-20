#pragma once
#include "ui/ISelectionAction.h"
#include <functional>

class CopyAction : public ISelectionAction {
public:
    std::string Label() const override { return "Copy"; }
    void Execute(const std::u32string& text) override;
};

class SaveToFileAction : public ISelectionAction {
public:
    std::string Label() const override { return "Save to File..."; }
    void Execute(const std::u32string& text) override;
};

class WebSearchAction : public ISelectionAction {
public:
    explicit WebSearchAction(std::string baseUrl);

    std::string Label() const override { return "Search the Web"; }
    void Execute(const std::u32string& text) override;
    void SetUrl(std::string url) { baseUrl_ = std::move(url); }

private:
    std::string baseUrl_;
};

// Opens the in-terminal search bar with the selected text as the initial query.
// The callback is supplied by the caller (UIManager) so this class stays
// independent of any specific UI framework type.
class FindInTerminalAction : public ISelectionAction {
public:
    using Callback = std::function<void(const std::u32string&)>;
    explicit FindInTerminalAction(Callback cb) : cb_(std::move(cb)) {}

    std::string Label() const override { return "Find in Terminal"; }
    void Execute(const std::u32string& text) override { if (cb_) cb_(text); }

private:
    Callback cb_;
};

// Sends the selected text back through the InputRouter as paste input,
// allowing commands from the scrollback to be re-executed.
class PasteSelectionAction : public ISelectionAction {
public:
    using Callback = std::function<void(const std::u32string&)>;
    explicit PasteSelectionAction(Callback cb) : cb_(std::move(cb)) {}

    std::string Label() const override { return "Paste Selection"; }
    void Execute(const std::u32string& text) override { if (cb_) cb_(text); }

private:
    Callback cb_;
};
