#include "webapi/ring_upload.h"

namespace swan {
namespace api {

bool RingStager::stage(std::string_view body, std::string* err) {
    if (body.empty()) {
        if (err) *err = "empty upload";
        return false;
    }
    if (body.size() > RING_UPLOAD_MAX) {
        if (err) {
            *err = "upload is " + std::to_string(body.size()) + " bytes; limit is " +
                   std::to_string(RING_UPLOAD_MAX);
        }
        return false;
    }

    // Validate into a throwaway set.  load_json parses, enforces the 50-slot
    // physical size, and runs the per-column role assertion; on any failure it
    // leaves the candidate on the compiled fallback and returns false.
    auto candidate = std::make_unique<RingSet>();
    if (!candidate->load_json(body, err)) return false;

    const std::lock_guard<std::mutex> lock(mu_);
    staged_ = std::move(candidate);
    accepted_body_.assign(body);
    pending_.store(true, std::memory_order_release);
    return true;
}

bool RingStager::apply_pending() {
    if (!pending_.load(std::memory_order_acquire)) return false;
    const std::lock_guard<std::mutex> lock(mu_);
    if (!staged_) {
        pending_.store(false, std::memory_order_release);
        return false;
    }
    live_ = *staged_;  // renderers hold a reference to `live_`, not to the table
    staged_.reset();
    pending_.store(false, std::memory_order_release);
    return true;
}

std::string RingStager::take_accepted_body() {
    const std::lock_guard<std::mutex> lock(mu_);
    return std::move(accepted_body_);
}

}  // namespace api
}  // namespace swan
