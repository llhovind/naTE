#include "fs/LinkResolver.h"
#include "fs/RemotePath.h"

#include <utility>

namespace term::fs {

namespace {

// Reads one stat outcome as an answer about the link that produced it.
//
// Only a definite "there is nothing there" makes a link broken. Every other
// failure — permission denied on a parent, a session that just died, a server
// that answered something unrecognised — means the question was not answered,
// and saying "broken" would put a red row in front of the user on the strength
// of a guess.
LinkTarget Classify(const transport::FileInfo& info, const transport::FsError& err)
{
    if (err.Ok())                                      return info.isDir
                                                            ? LinkTarget::Directory
                                                            : LinkTarget::File;
    if (err.code == transport::FsErrorCode::NoSuchFile) return LinkTarget::Broken;
    return LinkTarget::Unresolved;
}

} // namespace

// The work of one Resolve call, shared by every lookup it issued so the last
// one to finish can deliver the result.
struct LinkResolver::Batch {
    std::string                 directory;
    std::vector<std::string>    names;
    size_t                      next        = 0;  // first name not yet issued
    size_t                      outstanding = 0;  // lookups at the transport
    std::vector<LinkResolution> results;
    BatchCallback               onDone;
    uint64_t                    generation = 0;

    bool Finished() const noexcept
    {
        return next >= names.size() && outstanding == 0;
    }
};

LinkResolver::LinkResolver(transport::IRemoteFileSystem& remote, Dispatcher dispatch)
    : remote_(remote)
    , guard_(std::move(dispatch))
{}

// Callbacks still held by the transport check the guard before touching us,
// so destruction must happen on the same thread the dispatcher posts to.
LinkResolver::~LinkResolver() = default;

void LinkResolver::Cancel()
{
    ++generation_;
}

void LinkResolver::Resolve(std::string directory, std::vector<std::string> names,
                           BatchCallback onDone)
{
    Cancel();

    auto batch = std::make_shared<Batch>();
    batch->directory  = std::move(directory);
    batch->names      = std::move(names);
    batch->onDone     = std::move(onDone);
    batch->generation = generation_;
    batch->results.reserve(batch->names.size());

    Pump(batch);
}

void LinkResolver::Pump(const std::shared_ptr<Batch>& batch)
{
    if (batch->generation != generation_) return;

    // The port allows a callback to fire before the call that started it has
    // returned, and a dispatcher is allowed to run its work inline — so a
    // completion can land in the middle of the loop below. The loop's own
    // condition picks up the slot it freed, which leaves the nested call with
    // nothing to do; recursing instead would grow the stack with the directory.
    if (!pumping_) {
        pumping_ = true;
        IssueLookups(batch);
        pumping_ = false;
    }

    if (!batch->Finished()) return;

    // Moved out before the call: the callback may start another batch, and it
    // must not be running against a callback this one still owns.
    auto done = std::move(batch->onDone);
    if (done) done(std::move(batch->results));
}

void LinkResolver::IssueLookups(const std::shared_ptr<Batch>& batch)
{
    while (batch->outstanding < kMaxConcurrentLinkLookups &&
           batch->next < batch->names.size()) {
        const std::string name = batch->names[batch->next++];
        ++batch->outstanding;

        auto ctx = guard_.For(this);
        // Stat follows the link, which is exactly the question being asked:
        // what is at the other end, if anything.
        remote_.Stat(path::Join(batch->directory, name),
                     [ctx, batch, name](transport::FileInfo info,
                                        transport::FsError err) {
            ctx.Post([batch, name, info, err = std::move(err)](
                         LinkResolver& r) mutable {
                if (batch->generation != r.generation_) return;
                --batch->outstanding;
                batch->results.push_back({name, Classify(info, err)});
                r.Pump(batch);
            });
        });
    }
}

} // namespace term::fs
