#pragma once
#include <string>

// Extension point for actions that operate on selected terminal text.
// Register concrete implementations via SelectionActionRegistry.
class ISelectionAction {
public:
    virtual ~ISelectionAction() = default;

    // Short human-readable label shown in the context menu.
    virtual std::string Label() const = 0;

    // Perform the action. text is the current selection in UTF-32.
    virtual void Execute(const std::u32string& text) = 0;
};
