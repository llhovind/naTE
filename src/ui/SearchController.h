#pragma once
#include <optional>
#include <string>
#include <vector>
#include <wx/timer.h>
#include "ui/SearchMatch.h"

class DocLayout;
class TerminalPanel;
class SearchBar;

class SearchController : public wxEvtHandler {
public:
    SearchController(DocLayout& layout, TerminalPanel& panel);

    void SetBar(SearchBar* bar);

    void SetQuery(const std::u32string& query);
    // Pre-populate the search bar with query and run the search immediately.
    void SetInitialQuery(const std::u32string& query);
    void NextMatch();
    void PrevMatch();
    void Clear();

    // Called by TerminalPanel::OnDocumentUpdate() whenever the document changes.
    // Triggers debounced incremental search if a query is active.
    void NotifyDocumentChanged();

    size_t MatchCount()   const { return matches_.size(); }
    size_t CurrentIndex() const { return currentIdx_; }  // 0-based

private:
    void RebuildMatches();
    void NavigateTo(size_t idx);
    void PushToLayout();

    // Incremental: scan only new tail lines, update matches_ in-place.
    void AppendSearch();
    // Full rebuild with viewport-anchor preservation (used when doc shrinks).
    void RebuildAndStay();

    void OnDebounceTimer(wxTimerEvent&);

    DocLayout&     layout_;
    TerminalPanel& panel_;
    SearchBar*     bar_ = nullptr;

    std::u32string            query_;
    std::u32string            foldedQuery_;
    std::vector<SearchMatch>  matches_;
    size_t                    currentIdx_        = 0;
    size_t                    appendedUpToLine_  = 0;
    bool                      dirty_             = false;
    wxTimer                   debounceTimer_;
};
