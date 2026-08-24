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

    // Validate into a throwaway set, through the STREAMING loader: it enforces
    // the same 50-slot physical size and the same per-column role assertion,
    // and on any failure it leaves the candidate on the compiled fallback -
    // but it never builds a document.
    //
    // That last part is the whole reason this path is different from the
    // command path.  A json::Value is ~64 bytes against a 2-byte minimum
    // token, so the DOM parser's node cap (700, against a real ring.json's
    // 535) was the only thing standing between an upload and an allocation
    // failure - and with exceptions off, an allocation failure is abort(),
    // which is a reboot.  The streaming loader's memory does not depend on the
    // document at all, so the cap stops being load-bearing.
    auto candidate = std::make_unique<RingSet>();
    if (!candidate->load_json_streaming(body, err)) return false;

    const std::lock_guard<std::mutex> lock(mu_);
    staged_ = std::move(candidate);
    accepted_body_.assign(body);
    pending_.store(true, std::memory_order_release);
    return true;
}

RingSet RingStager::snapshot() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return live_;  // shared_ptr copies: the tables outlive the next swap
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
