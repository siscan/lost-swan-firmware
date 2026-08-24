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

void LogRing::push(const char* data, std::size_t len, bool truncated) {
    if (buf_ == nullptr || cap_ < HDR + 4) return;
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) --len;
    if (len == 0) return;

    if (len > LOG_LINE_MAX || (truncated && len + 3 > LOG_LINE_MAX)) {
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

std::size_t LogRing::start_of_capped(std::size_t max_bytes, std::size_t& start_i) const {
    // The newest line that still fits, so a capped read returns the END of the
    // log.  Returning the oldest bytes and calling it "the log" is the wrong
    // half.
    std::size_t start = tail_;
    start_i = 0;
    if (max_bytes == 0 || used_ + lines_ <= max_bytes) return start;

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
    return start;
}

std::size_t LogRing::read_into(char* dst, std::size_t dst_cap, std::size_t max_bytes) const {
    if (dst == nullptr || dst_cap == 0 || lines_ == 0) return 0;

    std::size_t start_i = 0;
    std::size_t p = start_of_capped(max_bytes, start_i);
    std::size_t out = 0;
    for (std::size_t i = start_i; i < lines_; ++i) {
        const std::size_t len =
            static_cast<std::size_t>(static_cast<unsigned char>(buf_[p])) |
            (static_cast<std::size_t>(static_cast<unsigned char>(buf_[(p + 1) % cap_])) << 8);
        p = (p + HDR) % cap_;
        if (out + len + 1 > dst_cap) break;          // the buffer wins, always
        const std::size_t first = std::min(len, cap_ - p);
        std::memcpy(dst + out, buf_ + p, first);
        if (first < len) std::memcpy(dst + out + first, buf_, len - first);
        out += len;
        dst[out++] = '\n';
        p = (p + len) % cap_;
    }
    return out;
}

std::string LogRing::read(std::size_t max_bytes) const {
    std::string out;
    if (lines_ == 0) return out;
    std::size_t cap = used_ + lines_;
    if (max_bytes != 0 && cap > max_bytes) cap = max_bytes + LOG_LINE_MAX + 1;
    out.resize(cap);
    out.resize(read_into(&out[0], cap, max_bytes));
    return out;
}

void LogRing::clear() {
    head_ = tail_ = used_ = lines_ = 0;
}

}  // namespace journal
}  // namespace swan
