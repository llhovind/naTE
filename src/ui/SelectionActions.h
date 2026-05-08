#pragma once
#include "ui/ISelectionAction.h"

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
    std::string Label() const override { return "Search the Web"; }
    void Execute(const std::u32string& text) override;
};
