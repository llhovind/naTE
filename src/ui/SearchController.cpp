#include "ui/SearchController.h"
#include "layout/DocLayout.h"
#include "layout/SearchMatch.h"
#include "ui/TerminalPanel.h"
#include "ui/SearchBar.h"
#include <algorithm>

SearchController::SearchController(DocLayout& layout, TerminalPanel& panel)
    : layout_(layout), panel_(panel)
{
    debounceTimer_.SetOwner(this);
    Bind(wxEVT_TIMER, &SearchController::OnDebounceTimer, this, debounceTimer_.GetId());
}

void SearchController::SetBar(SearchBar* bar) { bar_ = bar; }

void SearchController::SetInitialQuery(const std::u32string& query)
{
    if (bar_)
        bar_->SetInitialQuery(query);
    else
        SetQuery(query);
}

void SearchController::SetQuery(const std::u32string& query)
{
    query_ = query;
    if (query_.empty()) {
        Clear();
        return;
    }
    foldedQuery_ = SearchCaseFoldStr(query_);
    RebuildMatches();
    appendedUpToLine_ = (size_t)layout_.GetLineCount();
    currentIdx_ = matches_.empty() ? 0 : matches_.size() - 1;
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
    debounceTimer_.Stop();
    appendedUpToLine_ = 0;
    query_.clear();
    foldedQuery_.clear();
    matches_.clear();
    currentIdx_ = 0;
    PushToLayout();
    if (bar_) bar_->UpdateStatus(0, 0);
    panel_.Refresh();
}

void SearchController::NotifyDocumentChanged()
{
    if (query_.empty()) return;
    dirty_ = true;
    if (!debounceTimer_.IsRunning())
        debounceTimer_.StartOnce(250);
}

void SearchController::OnDebounceTimer(wxTimerEvent&)
{
    dirty_ = false;
    AppendSearch();
}

void SearchController::AppendSearch()
{
    const int lineCount = layout_.GetLineCount();

    // Document shrank (max-lines pruning deleted lines from the front) — fall back
    // to full rebuild with anchor-preservation so indices don't go stale.
    if (lineCount < (int)appendedUpToLine_) {
        RebuildAndStay();
        return;
    }

    // Re-scan from one before the watermark so a cursor line that was still
    // in-progress last tick gets re-covered correctly this tick.
    const size_t from = appendedUpToLine_ > 0 ? appendedUpToLine_ - 1 : 0;

    // Drop matches for lines at or after `from` — they may be stale.
    while (!matches_.empty() && matches_.back().lineIndex >= from)
        matches_.pop_back();

    // Append matches for the tail range.
    auto tail = layout_.SearchRange(foldedQuery_, from, (size_t)lineCount);
    matches_.insert(matches_.end(), tail.begin(), tail.end());
    appendedUpToLine_ = (size_t)lineCount;

    // Viewport policy: follow tail when at end, preserve position when scrolled up.
    if (!matches_.empty()) {
        if (layout_.IsAtEnd())
            currentIdx_ = matches_.size() - 1;
        else
            currentIdx_ = std::min(currentIdx_, matches_.size() - 1);
    } else {
        currentIdx_ = 0;
    }

    PushToLayout();
    if (bar_) bar_->UpdateStatus(matches_.empty() ? 0 : currentIdx_ + 1, matches_.size());
    panel_.Refresh();
}

void SearchController::RebuildAndStay()
{
    const auto anchor = matches_.empty()
        ? std::optional<SearchMatch>{}
        : std::make_optional(matches_[currentIdx_]);

    RebuildMatches();
    appendedUpToLine_ = (size_t)layout_.GetLineCount();

    if (matches_.empty()) {
        currentIdx_ = 0;
    } else if (layout_.IsAtEnd()) {
        currentIdx_ = matches_.size() - 1;
    } else if (anchor) {
        // Exact position match; fall back to nearest by line index.
        const auto it = std::find_if(matches_.begin(), matches_.end(),
            [&](const SearchMatch& m) {
                return m.lineIndex == anchor->lineIndex && m.colStart == anchor->colStart;
            });
        if (it != matches_.end()) {
            currentIdx_ = (size_t)std::distance(matches_.begin(), it);
        } else {
            size_t best = 0;
            for (size_t i = 1; i < matches_.size(); ++i) {
                if (std::abs((int)matches_[i].lineIndex - (int)anchor->lineIndex) <
                    std::abs((int)matches_[best].lineIndex - (int)anchor->lineIndex))
                    best = i;
            }
            currentIdx_ = best;
        }
    } else {
        currentIdx_ = matches_.size() - 1;
    }

    PushToLayout();
    if (bar_) bar_->UpdateStatus(matches_.empty() ? 0 : currentIdx_ + 1, matches_.size());
    panel_.Refresh();
}

void SearchController::RebuildMatches()
{
    matches_ = layout_.Search(foldedQuery_);
}

void SearchController::NavigateTo(size_t idx)
{
    if (idx < matches_.size()) {
        const SearchMatch& m = matches_[idx];

        // Vertical: place the match line ~1/3 from the top of the viewport.
        const int lineRow  = layout_.GetVisualRowForDocLine((int)m.lineIndex);
        const int viewRows = layout_.GetViewportRows();
        layout_.SetTopVisualRow(std::max(0, lineRow - viewRows / 3));

        // Horizontal: only relevant when wrap mode is off.
        if (!layout_.GetWrapMode()) {
            const int matchCol = (int)m.colStart;
            const int leftCol  = layout_.GetLeftCol();
            const int viewCols = layout_.GetViewportCols();
            if (matchCol < leftCol || matchCol + (int)m.colLen > leftCol + viewCols)
                layout_.SetLeftCol(std::max(0, matchCol - viewCols / 4));
        }
    }
    if (bar_)
        bar_->UpdateStatus(matches_.empty() ? 0 : currentIdx_ + 1, matches_.size());
    panel_.SyncScrollbars();
    panel_.Refresh();
}

void SearchController::PushToLayout()
{
    layout_.SetSearchState(matches_, currentIdx_);
}
