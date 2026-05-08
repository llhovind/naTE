#pragma once
#include <string>
#include <vector>
#include "ui/SearchMatch.h"

class DocLayout;
class TerminalPanel;
class SearchBar;

class SearchController {
public:
    SearchController(DocLayout& layout, TerminalPanel& panel);

    void SetBar(SearchBar* bar);

    void SetQuery(const std::u32string& query);
    void NextMatch();
    void PrevMatch();
    void Clear();

    size_t MatchCount()   const { return matches_.size(); }
    size_t CurrentIndex() const { return currentIdx_; }  // 0-based

private:
    void RebuildMatches();
    void NavigateTo(size_t idx);
    void PushToLayout();

    DocLayout&     layout_;
    TerminalPanel& panel_;
    SearchBar*     bar_ = nullptr;

    std::u32string           query_;
    std::vector<SearchMatch> matches_;
    size_t                   currentIdx_ = 0;
};
