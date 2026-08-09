#include "fs/RemoteDeleter.h"
#include "fs/RemotePath.h"

#include <algorithm>
#include <deque>
#include <memory>

namespace term::fs {

RemoteDeleter::RemoteDeleter(transport::IRemoteFileSystem& remote,
                             Dispatcher dispatch)
    : remote_(remote)
    , guard_(std::move(dispatch))
{}

// Callbacks still held by the transport check the guard before touching us,
// so destruction must happen on the same thread the dispatcher posts to.
RemoteDeleter::~RemoteDeleter() = default;

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

void RemoteDeleter::Plan(const DeleteTarget& target,
                         std::function<void(DeletePlan)> onDone)
{
    const std::string path = target.path;

    // A symlink is one unlink whatever it points at, so there is nothing to
    // walk — and walking it would be the bug this rule exists to prevent.
    if (!target.isDir || target.isSymlink) {
        DeletePlan plan;
        plan.steps.push_back({path, false, 0});
        plan.fileCount  = 1;
        plan.totalBytes = target.size;
        if (onDone) onDone(std::move(plan));
        return;
    }

    struct PendingDir {
        std::string path;
        int         depth = 0;
    };

    struct Walk {
        std::deque<PendingDir>          pending;
        DeletePlan                      plan;
        std::function<void(DeletePlan)> onDone;
    };

    auto walk = std::make_shared<Walk>();
    walk->pending.push_back({path, 0});
    walk->onDone = std::move(onDone);
    // The root directory itself is the final step.
    walk->plan.steps.push_back({path, true, 0});
    walk->plan.dirCount = 1;

    auto step = std::make_shared<std::function<void()>>();
    auto ctx  = guard_.For(this);

    *step = [this, ctx, walk, step]() {
        if (walk->pending.empty()) {
            // Deepest first: a directory is only removable once the entries
            // it contains — which are all at a greater depth — are gone.
            std::stable_sort(walk->plan.steps.begin(), walk->plan.steps.end(),
                             [](const DeleteStep& a, const DeleteStep& b) {
                                 return a.depth > b.depth;
                             });
            auto done = std::move(walk->onDone);
            if (done) done(std::move(walk->plan));
            return;
        }

        const PendingDir dir = walk->pending.front();
        walk->pending.pop_front();

        remote_.List(dir.path, [ctx, walk, step, dir](
                                   std::vector<transport::FileInfo> entries,
                                   transport::FsError err) {
            ctx.Post([walk, step, dir, entries = std::move(entries),
                      err = std::move(err)](RemoteDeleter&) mutable {
                // Record the first failure but keep enumerating: the caller is
                // shown the incomplete plan and decides, rather than being
                // told nothing can be done.
                if (err.Failed() && walk->plan.error.Ok())
                    walk->plan.error = std::move(err);

                for (const auto& e : entries) {
                    const std::string child = path::Join(dir.path, e.name);
                    const int childDepth = dir.depth + 1;

                    // A symlink is unlinked where it stands, never descended.
                    const bool descend = e.isDir && !e.isSymlink &&
                                         childDepth < kMaxDeleteDepth;

                    walk->plan.steps.push_back(
                        {child, e.isDir && !e.isSymlink, childDepth});

                    if (e.isDir && !e.isSymlink) {
                        ++walk->plan.dirCount;
                        if (descend) walk->pending.push_back({child, childDepth});
                    } else {
                        ++walk->plan.fileCount;
                        walk->plan.totalBytes += e.size;
                    }
                }
                (*step)();
            });
        });
    };

    (*step)();
}

void RemoteDeleter::Plan(std::vector<DeleteTarget> targets,
                         std::function<void(DeletePlan)> onDone)
{
    struct Batch {
        std::vector<DeleteTarget>       targets;
        size_t                          index = 0;
        DeletePlan                      merged;
        std::function<void(DeletePlan)> onDone;
    };

    auto batch = std::make_shared<Batch>();
    batch->targets = std::move(targets);
    batch->onDone  = std::move(onDone);

    auto step = std::make_shared<std::function<void()>>();
    auto ctx  = guard_.For(this);

    *step = [this, ctx, batch, step]() {
        if (batch->index >= batch->targets.size()) {
            auto done = std::move(batch->onDone);
            if (done) done(std::move(batch->merged));
            return;
        }

        const DeleteTarget target = batch->targets[batch->index++];
        Plan(target,
             [ctx, batch, step](DeletePlan plan) {
                 ctx.Post([batch, step, plan = std::move(plan)](RemoteDeleter&) mutable {
                     auto& merged = batch->merged;
                     merged.steps.insert(merged.steps.end(),
                                         plan.steps.begin(), plan.steps.end());
                     merged.fileCount  += plan.fileCount;
                     merged.dirCount   += plan.dirCount;
                     merged.totalBytes += plan.totalBytes;
                     // Keep the first problem encountered; the confirmation
                     // only needs to say that the count is incomplete.
                     if (plan.error.Failed() && merged.error.Ok())
                         merged.error = std::move(plan.error);
                     (*step)();
                 });
             });
    };

    (*step)();
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void RemoteDeleter::Execute(DeletePlan plan,
                            std::function<void(size_t, size_t)> onProgress,
                            std::function<void(transport::FsError)> onDone)
{
    struct Run {
        DeletePlan                              plan;
        size_t                                  index = 0;
        std::function<void(size_t, size_t)>     onProgress;
        std::function<void(transport::FsError)> onDone;
    };

    auto run = std::make_shared<Run>();
    run->plan       = std::move(plan);
    run->onProgress = std::move(onProgress);
    run->onDone     = std::move(onDone);

    auto step = std::make_shared<std::function<void()>>();
    auto ctx  = guard_.For(this);

    *step = [this, ctx, run, step]() {
        if (run->index >= run->plan.steps.size()) {
            auto done = std::move(run->onDone);
            if (done) done(transport::FsError::Success());
            return;
        }

        const DeleteStep current = run->plan.steps[run->index];
        remote_.Remove(current.path, current.isDir,
            [ctx, run, step, current](transport::FsError err) {
                ctx.Post([run, step, current,
                          err = std::move(err)](RemoteDeleter&) mutable {
                    if (err.Failed()) {
                        // Stop here. Once one removal fails the tree above it
                        // cannot be emptied either, and continuing would bury
                        // the real cause under a run of "not empty" errors.
                        auto done = std::move(run->onDone);
                        if (done) done(std::move(err));
                        return;
                    }
                    ++run->index;
                    if (run->onProgress)
                        run->onProgress(run->index, run->plan.steps.size());
                    (*step)();
                });
            });
    };

    (*step)();
}

} // namespace term::fs
