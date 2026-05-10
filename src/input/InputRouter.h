#pragma once

#include <unordered_set>
#include <vector>
#include <memory>

#include "KeyEvent.hpp"

namespace term::input {

class InputTarget {
public:
    virtual ~InputTarget() = default;
    virtual void OnInput(const KeyEvent& event) = 0;
    virtual void Paste(const std::string& utf8) {}
};

class InputFilter {
public:
    virtual ~InputFilter() = default;

    // return false to drop event
    virtual bool Process(KeyEvent& event) = 0;
};

enum class InputMode {
    Focused,
    Broadcast
};

class InputRouter {
public:
    InputRouter() = default;
    ~InputRouter() = default;

    InputRouter(const InputRouter&) = delete;
    InputRouter& operator=(const InputRouter&) = delete;

    // --- Target management ---
    void AddTarget(InputTarget* target);
    void RemoveTarget(InputTarget* target);

    // --- Focus ---
    void SetFocused(InputTarget* target);
    InputTarget* GetFocused() const noexcept;

    // --- Selection (broadcast mode) ---
    void Select(InputTarget* target);
    void Deselect(InputTarget* target);
    void ClearSelection();

    bool IsSelected(InputTarget* target) const;
    bool HasSelection() const noexcept;

    // --- Mode ---
    void SetMode(InputMode mode) noexcept;
    InputMode GetMode() const noexcept;

    // --- Filters ---
    void AddFilter(std::shared_ptr<InputFilter> filter);
    void ClearFilters();

    // --- Dispatch ---
    void Send(KeyEvent event);
    void Paste(const std::string& utf8);

private:
    bool IsValidTarget(InputTarget* target) const;

private:
    std::unordered_set<InputTarget*> targets_;
    std::unordered_set<InputTarget*> selected_;

    std::vector<std::shared_ptr<InputFilter>> filters_;

    InputTarget* focused_ = nullptr;
    InputMode mode_ = InputMode::Focused;
};

} // namespace term::input

