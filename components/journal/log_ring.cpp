#include "journal/log_ring.h"

#include <algorithm>
#include <cstring>

namespace swan {
namespace journal {

namespace {
// Every stored line is prefixed with its length, so eviction can skip a whole
// line without scanning for a separator - a backtrace full of newlines would
// otherwise be evicted a fragment at a time.
constexpr std::size_t HDR = 2;   // uint16 little-endian length
}  // namespace

void LogRing::pop_oldest() {
    if (lines_ == 0) return;
    const std::size_t a = tail_;
    const std::size_t b = (tail_ + 1) % cap_;
    const std::size_t len = static_cast<std::size_t>(static_cast<unsigned char>(buf_[a])) |
                            (static_cast<std::size_t>(static_cast<unsigned char>(buf_[b])) << 8);
    tail_ = (tail_ + HDR + len) % cap_;
    used_ -= HDR + len;
    --lines_;
    ++dropped_;
}

void LogRing::push(const char* data, std::size_t len) {
    if (buf_ == nullptr || cap_ < HDR + 4) return;
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) --len;
    if (len == 0) return;

    bool truncated = false;
    if (len > LOG_LINE_MAX) {
        len = LOG_LINE_MAX - 3;
        truncated = true;
    }
    const std::size_t need = HDR + len + (truncated ? 3 : 0);
    if (need > cap_) return;                    // pathological capacity

    while (used_ + need > cap_) pop_oldest();

    const std::size_t total = len + (truncated ? 3 : 0);
    buf_[head_] = static_cast<char>(total & 0xFF);
    buf_[(head_ + 1) % cap_] = static_cast<char>((total >> 8) & 0xFF);
    head_ = (head_ + HDR) % cap_;

    const std::size_t first = std::min(len, cap_ - head_);
    std::memcpy(buf_ + head_, data, first);
    if (first < len) std::memcpy(buf_, data + first, len - first);
    head_ = (head_ + len) % cap_;

    if (truncated) {
        for (const char c : {'.', '.', '.'}) {
            buf_[head_] = c;
            head_ = (head_ + 1) % cap_;
        }
    }
    used_ += need;
    ++lines_;
}

std::string LogRing::read(std::size_t max_bytes) const {
    // Line offsets, oldest first, so a capped read can start from the newest
    // line that still fits.  A log view wants the END of the log; returning the
    // oldest bytes and calling it "the log" is the wrong half.
    std::string out;
    if (lines_ == 0) return out;

    std::size_t start = tail_;
    std::size_t start_i = 0;
    if (max_bytes != 0 && used_ + lines_ > max_bytes) {
        std::size_t p = tail_;
        std::size_t remaining = used_ + lines_;   // + one newline per line
        for (std::size_t i = 0; i < lines_; ++i) {
            const std::size_t len =
                static_cast<std::size_t>(static_cast<unsigned char>(buf_[p])) |
                (static_cast<std::size_t>(static_cast<unsigned char>(buf_[(p + 1) % cap_])) << 8);
            if (remaining <= max_bytes) break;
            remaining -= HDR + len + 1;
            p = (p + HDR + len) % cap_;
            start = p;
            start_i = i + 1;
        }
    }

    out.reserve(max_bytes == 0 ? used_ + lines_ : max_bytes);
    std::size_t p = start;
    for (std::size_t i = start_i; i < lines_; ++i) {
        const std::size_t len =
            static_cast<std::size_t>(static_cast<unsigned char>(buf_[p])) |
            (static_cast<std::size_t>(static_cast<unsigned char>(buf_[(p + 1) % cap_])) << 8);
        p = (p + HDR) % cap_;
        const std::size_t first = std::min(len, cap_ - p);
        out.append(buf_ + p, first);
        if (first < len) out.append(buf_, len - first);
        out.push_back('\n');
        p = (p + len) % cap_;
    }
    return out;
}

void LogRing::clear() {
    head_ = tail_ = used_ = lines_ = 0;
}

}  // namespace journal
}  // namespace swan
