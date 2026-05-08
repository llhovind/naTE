#include "ui/SelectionActionRegistry.h"
#include <wx/menu.h>

void SelectionActionRegistry::Register(std::unique_ptr<ISelectionAction> action)
{
    actions_.push_back(std::move(action));
}

void SelectionActionRegistry::PopulateMenu(wxMenu& menu, int baseId) const
{
    for (int i = 0; i < (int)actions_.size(); ++i)
        menu.Append(baseId + i, actions_[i]->Label());
}

void SelectionActionRegistry::Execute(int index, const std::u32string& text)
{
    if (index >= 0 && index < (int)actions_.size())
        actions_[index]->Execute(text);
}

int SelectionActionRegistry::Count() const
{
    return (int)actions_.size();
}
