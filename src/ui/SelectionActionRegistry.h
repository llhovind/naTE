#pragma once
#include "ui/ISelectionAction.h"
#include <memory>
#include <vector>

class wxMenu;

class SelectionActionRegistry {
public:
    void Register(std::unique_ptr<ISelectionAction> action);

    // Append one menu item per registered action, starting at baseId.
    void PopulateMenu(wxMenu& menu, int baseId) const;

    // Invoke actions_[index] with the provided text. No-op if index is out of range.
    void Execute(int index, const std::u32string& text);

    // Update the URL on the registered WebSearchAction, if any.
    void UpdateWebSearchUrl(const std::string& url);

    int Count() const;

private:
    std::vector<std::unique_ptr<ISelectionAction>> actions_;
};
