#include "InputRouter.h"

namespace term::input {

// --- Target management ---

void InputRouter::AddTarget(InputTarget* target) {
    if (!target) return;
    targets_.insert(target);
}

void InputRouter::RemoveTarget(InputTarget* target) {
    if (!target) return;

    targets_.erase(target);
    selected_.erase(target);

    if (focused_ == target) {
        focused_ = nullptr;
    }
}

// --- Focus ---

void InputRouter::SetFocused(InputTarget* target) {
    if (target && !IsValidTarget(target)) return;
    focused_ = target;
}

InputTarget* InputRouter::GetFocused() const noexcept {
    return focused_;
}

// --- Selection ---

void InputRouter::Select(InputTarget* target) {
    if (!IsValidTarget(target)) return;
    selected_.insert(target);
}

void InputRouter::Deselect(InputTarget* target) {
    selected_.erase(target);
}

void InputRouter::ClearSelection() {
    selected_.clear();
}

bool InputRouter::IsSelected(InputTarget* target) const {
    return selected_.find(target) != selected_.end();
}

// --- Mode ---

void InputRouter::SetMode(InputMode mode) noexcept {
    mode_ = mode;
}

InputMode InputRouter::GetMode() const noexcept {
    return mode_;
}

// --- Filters ---

void InputRouter::AddFilter(std::shared_ptr<InputFilter> filter) {
    if (filter) {
        filters_.push_back(std::move(filter));
    }
}

void InputRouter::ClearFilters() {
    filters_.clear();
}

// --- Dispatch ---

void InputRouter::Send(KeyEvent event) {
    // Apply filters
    for (auto& f : filters_) {
        if (!f->Process(event)) {
            return; // dropped
        }
    }

    switch (mode_) {
    case InputMode::Focused:
        if (focused_) {
            focused_->OnInput(event);
        }
        break;

    case InputMode::Broadcast:
        for (auto* target : selected_) {
            if (target) {
                target->OnInput(event);
            }
        }
        break;
    }
}

void InputRouter::Paste(const std::string& utf8) {
    if (utf8.empty()) return;

    switch (mode_) {
    case InputMode::Focused:
        if (focused_) {
            focused_->Paste(utf8);
        }
        break;

    case InputMode::Broadcast:
        for (auto* target : selected_) {
            if (target) {
                target->Paste(utf8);
            }
        }
        break;
    }
}

// --- Internal ---

bool InputRouter::IsValidTarget(InputTarget* target) const {
    return target && targets_.find(target) != targets_.end();
}

} // namespace term::input
