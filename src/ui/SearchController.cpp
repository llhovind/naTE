#include "ui/SearchController.h"
#include "ui/DocLayout.h"
#include "ui/TerminalPanel.h"
#include "ui/SearchBar.h"
#include <algorithm>

static char32_t CaseFold(char32_t c)
{
    return (c >= U'A' && c <= U'Z') ? c + (U'a' - U'A') : c;
}

static std::u32string FoldedStr(const std::u32string& s)
{
    std::u32string r;
    r.reserve(s.size());
    for (char32_t c : s)
        r.push_back(CaseFold(c));
    return r;
}

SearchController::SearchController(DocLayout& layout, TerminalPanel& panel)
    : layout_(layout), panel_(panel)
{}

void SearchController::SetBar(SearchBar* bar) { bar_ = bar; }

void SearchController::SetQuery(const std::u32string& query)
{
    query_ = query;
    if (query_.empty()) {
        Clear();
        return;
    }
    RebuildMatches();
    currentIdx_ = 0;
    PushToLayout();
    if (!matches_.empty())
        NavigateTo(currentIdx_);
    else {
        if (bar_) bar_->UpdateStatus(0, 0);
        panel_.Refresh();
    }
}

void SearchController::NextMatch()
{
    if (matches_.empty()) return;
    currentIdx_ = (currentIdx_ + 1) % matches_.size();
    PushToLayout();
    NavigateTo(currentIdx_);
}

void SearchController::PrevMatch()
{
    if (matches_.empty()) return;
    currentIdx_ = (currentIdx_ == 0) ? matches_.size() - 1 : currentIdx_ - 1;
    PushToLayout();
    NavigateTo(currentIdx_);
}

void SearchController::Clear()
{
    query_.clear();
    matches_.clear();
    currentIdx_ = 0;
    PushToLayout();
    if (bar_) bar_->UpdateStatus(0, 0);
    panel_.Refresh();
}

void SearchController::RebuildMatches()
{
    matches_ = layout_.Search(FoldedStr(query_));
}

void SearchController::NavigateTo(size_t idx)
{
    if (idx < matches_.size()) {
        const int lineRow  = layout_.GetVisualRowForDocLine((int)matches_[idx].lineIndex);
        const int viewRows = layout_.GetViewportRows();
        layout_.SetTopVisualRow(std::max(0, lineRow - viewRows / 3));
    }
    if (bar_)
        bar_->UpdateStatus(matches_.empty() ? 0 : currentIdx_ + 1, matches_.size());
    panel_.Refresh();
}

void SearchController::PushToLayout()
{
    layout_.SetSearchState(matches_, currentIdx_);
}
