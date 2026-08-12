#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace term::fs {

// Marshals a callback onto the thread that owns an object. The UI supplies
// wxTheApp->CallAfter; tests supply an executor they can drain deliberately.
//
// This is what keeps the fs/ layer free of locks: collaborators such as
// IRemoteFileSystem call back on a worker thread, everything is bounced
// through a Dispatcher first, and so the owning object's state is only ever
// touched from one thread.
using Dispatcher = std::function<void(std::function<void()>)>;

// Everything a worker-thread callback needs to get safely back to its owner.
//
// Captured *by value* into callbacks handed to a collaborator, so the callback
// never touches the owner before the liveness check has passed on the owning
// thread. Capturing a bare `this` would mean dereferencing a possibly-destroyed
// object just to reach its dispatcher.
template <typename Owner>
struct GuardedContext {
    Dispatcher                       post;
    std::weak_ptr<std::atomic<bool>> alive;
    Owner*                           self = nullptr;

    bool Alive() const
    {
        const auto flag = alive.lock();
        return flag && flag->load(std::memory_order_acquire);
    }

    // Posts fn to the owning thread, invoking it with the owner only if the
    // owner is still alive when it gets there.
    template <typename Fn>
    void Post(Fn fn) const
    {
        auto ctx = *this;
        post([ctx, fn = std::move(fn)]() mutable {
            if (!ctx.Alive()) return;
            fn(*ctx.self);
        });
    }
};

// Holds the liveness flag and mints contexts from it.
//
// Composed as a member rather than inherited, so an owner opts into the
// behaviour without also inheriting an identity. The owner must be destroyed
// on the same thread the dispatcher posts to — that ordering is what makes the
// flag a sufficient guard.
class DispatchGuard {
public:
    explicit DispatchGuard(Dispatcher dispatch)
        : dispatch_(std::move(dispatch))
        , alive_(std::make_shared<std::atomic<bool>>(true))
    {}

    ~DispatchGuard() { Retire(); }

    // Retires outstanding callbacks now, rather than at destruction.
    //
    // For an owner that can be stopped and then linger — one told to quit but
    // still being joined, or still held by whoever is about to delete it — the
    // callbacks have to go dead at the stop, not at the eventual destruction.
    // Idempotent, and safe from any thread: the flag is the only thing touched.
    void Retire() noexcept { alive_->store(false, std::memory_order_release); }

    DispatchGuard(const DispatchGuard&)            = delete;
    DispatchGuard& operator=(const DispatchGuard&) = delete;

    template <typename Owner>
    GuardedContext<Owner> For(Owner* owner) const
    {
        return GuardedContext<Owner>{dispatch_, alive_, owner};
    }

    // Posts unconditionally, for work that does not touch the owner.
    void Post(std::function<void()> fn) const { dispatch_(std::move(fn)); }

private:
    Dispatcher                        dispatch_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

} // namespace term::fs
