#include "pms/link/frames.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pms::link {

std::vector<std::byte> encode_frame(const std::span<const std::byte> payload) {
    if (payload.size() < min_frame || payload.size() > max_frame) {
        throw std::length_error("frame payload outside 16..65535 bytes");
    }
    std::vector<std::byte> frame;
    frame.reserve(payload.size() + 2);
    frame.push_back(static_cast<std::byte>(payload.size() >> 8U));
    frame.push_back(static_cast<std::byte>(payload.size() & 0xFFU));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

FrameDecoder::FrameDecoder(const std::size_t max_payload) noexcept : max_payload_{max_payload} {}

void FrameDecoder::set_max_payload(const std::size_t max_payload) noexcept {
    max_payload_ = max_payload;
}

bool FrameDecoder::feed(std::span<const std::byte> bytes) {
    if (failed_) {
        return false;
    }
    while (!bytes.empty()) {
        if (header_size_ < header_.size()) {
            if (header_size_ == 0 && max_payload_ <= max_preauth_frame && !ready_.empty()) {
                return fail();  // before the handshake completes, exactly one frame
            }
            header_[header_size_++] = bytes.front();
            bytes = bytes.subspan(1);
            if (header_size_ < header_.size()) {
                continue;
            }
            expected_ = (std::to_integer<std::size_t>(header_[0]) << 8U) | std::to_integer<std::size_t>(header_[1]);
            if (expected_ < min_frame || expected_ > max_payload_) {
                return fail();
            }
            partial_.reserve(expected_);  // bounded by the cap checked just above
        }
        const auto chunk = bytes.first(std::min(expected_ - partial_.size(), bytes.size()));
        partial_.insert(partial_.end(), chunk.begin(), chunk.end());
        bytes = bytes.subspan(chunk.size());
        if (partial_.size() == expected_) {
            ready_.push_back(std::exchange(partial_, {}));
            header_size_ = 0;
        }
    }
    return true;
}

std::optional<std::vector<std::byte>> FrameDecoder::next() {
    if (failed_ || ready_.empty()) {
        return std::nullopt;
    }
    auto frame = std::move(ready_.front());
    ready_.pop_front();
    return frame;
}

bool FrameDecoder::fail() noexcept {
    failed_ = true;
    partial_ = std::vector<std::byte>{};
    ready_.clear();
    return false;
}

bool FrameDecoder::failed() const noexcept {
    return failed_;
}

}  // namespace pms::link
