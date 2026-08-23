// Ring upload staging (spec 4 / ring_store's threading contract).
//
// The HTTP task validates an uploaded ring.json entirely into a STAGING copy
// and queues the swap; the modes task applies it at a tick boundary.  The
// running table is never touched by the HTTP task, so a malformed, truncated,
// oversized or wrong-sized upload leaves the display exactly as it was.
//
// Pure: no IDF.  The target adds persistence around it (write the file only
// after validation succeeds).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "ring/ring_runtime.h"
#include "webapi/api.h"

namespace swan {
namespace api {

class RingStager : public RingStaging, public RingSource {
public:
    // `live` is the table the renderers use; only apply_pending() writes it,
    // and only the modes task may call that.
    explicit RingStager(RingSet& live) : live_(live) {}

    // HTTP task.  Validates into a staging RingSet; on success stores it and
    // raises the pending flag.  Never touches `live_`.
    bool stage(std::string_view body, std::string* err) override;

    // Modes task ONLY.  Swaps a staged table in if one is waiting; returns
    // true when it did.  The swap itself is done under mu_ so a concurrent
    // snapshot() cannot observe half-assigned pointers; the modes task's own
    // renderers are safe because they run on this same task.  Callers must
    // also force a re-render afterwards - the drums are parked on slots chosen
    // from the OLD table and nothing else will move them.
    bool apply_pending();

    // Any task.  A cheap copy taken under the swap lock, which pins the live
    // tables for as long as the returned set lives - see RingSource.  Every
    // reader outside the modes task MUST come through here: apply_pending()
    // drops the last reference to the outgoing tables and frees them, and a
    // raw `const RingTable&` held across a response would be reading freed
    // heap the moment an upload lands.
    RingSet snapshot() const override;

    bool pending() const { return pending_.load(std::memory_order_acquire); }

    // Set by stage() so the target can persist the accepted bytes AFTER
    // validation - a rejected upload never reaches the filesystem.
    std::string take_accepted_body();

private:
    RingSet& live_;
    mutable std::mutex mu_;
    std::unique_ptr<RingSet> staged_;
    std::string accepted_body_;
    std::atomic<bool> pending_{false};
};

}  // namespace api
}  // namespace swan
