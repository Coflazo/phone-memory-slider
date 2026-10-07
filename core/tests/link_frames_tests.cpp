#include <vector>

#include "pms/link/frames.hpp"
#include "test_harness.hpp"

void run_link_frames_tests() {
    using namespace pms::link;
    std::vector<std::byte> payload(300, std::byte{7});
    const auto f = encode_frame(payload);
    pms_check(f.size() == 302 && f[0] == std::byte{0x01} && f[1] == std::byte{0x2C}, "u16 BE length prefix");
    FrameDecoder d(max_frame);
    pms_check(d.feed(std::span(f).subspan(0, 1)) && !d.next(), "partial header waits");
    pms_check(d.feed(std::span(f).subspan(1)), "rest feeds");
    const auto got = d.next();
    pms_check(got && *got == payload, "frame reassembled");
    FrameDecoder pre(max_preauth_frame);
    const std::vector<std::byte> huge{std::byte{0xFF}, std::byte{0xFF}};
    pms_check(!pre.feed(huge) && pre.failed(), "pre-auth frames above 512 bytes fail without buffering");
    bool threw = false;
    try { (void)encode_frame(std::vector<std::byte>(70000)); } catch (const std::length_error&) { threw = true; }
    pms_check(threw, "oversized frames are refused at encode");

    // One read can carry several frames; every header is checked as it arrives.
    auto two = encode_frame(std::vector<std::byte>{std::byte{1}});
    const auto second = encode_frame(std::vector<std::byte>(512, std::byte{2}));
    two.insert(two.end(), second.begin(), second.end());
    FrameDecoder many(max_preauth_frame);
    pms_check(many.feed(two) && many.next().value().size() == 1 && many.next().value().size() == 512 && !many.next(), "frames split in order");
    two.push_back(std::byte{0x02});
    two.push_back(std::byte{0x01});
    FrameDecoder over(max_preauth_frame);
    pms_check(!over.feed(two) && over.failed() && !over.next() && !over.feed(f), "a bad header later in a read fails and stays failed");
}
