#include "ui/SelectionActionRegistry.h"
#include "ui/SelectionActions.h"
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

void SelectionActionRegistry::UpdateWebSearchUrl(const std::string& url)
{
    for (auto& action : actions_) {
        if (auto* ws = dynamic_cast<WebSearchAction*>(action.get())) {
            ws->SetUrl(url);
            return;
        }
    }
}

int SelectionActionRegistry::Count() const
{
    return (int)actions_.size();
}
