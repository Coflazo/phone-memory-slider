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

    // Every frame carries a Noise message (at least 16 bytes), so a flood of empty frames fails at once.
    FrameDecoder zeros(max_frame);
    pms_check(!zeros.feed(std::vector<std::byte>(64, std::byte{0})) && zeros.failed() && !zeros.next(), "a stream of 00 00 fails");
    threw = false;
    try { (void)encode_frame(std::vector<std::byte>(15)); } catch (const std::length_error&) { threw = true; }
    pms_check(threw, "frames shorter than 16 bytes are refused at encode");

    // The pre-auth cap, then the transport cap.
    const auto big = encode_frame(std::vector<std::byte>(600, std::byte{3}));
    FrameDecoder handshake(max_preauth_frame);
    pms_check(!handshake.feed(big) && handshake.failed(), "a 600-byte frame fails at the 512-byte pre-auth cap");
    FrameDecoder session(max_preauth_frame);
    session.set_max_payload(max_frame);
    pms_check(session.feed(big) && session.next().value().size() == 600, "the same frame passes after set_max_payload(max_frame)");

    // Exactly one frame before the handshake completes.
    auto two = encode_frame(std::vector<std::byte>(16, std::byte{1}));
    const auto second = encode_frame(std::vector<std::byte>(512, std::byte{2}));
    two.insert(two.end(), second.begin(), second.end());
    FrameDecoder pre_two(max_preauth_frame);
    pms_check(!pre_two.feed(two) && pre_two.failed() && !pre_two.next() && !pre_two.feed(f), "two pre-auth frames in one feed fail and stay failed");
    FrameDecoder taken(max_preauth_frame);
    pms_check(taken.feed(std::span(two).first(18)) && taken.next().value().size() == 16 &&
                  taken.feed(std::span(two).subspan(18)) && taken.next().value().size() == 512,
              "a pre-auth frame taken by next() frees the slot");

    // After the handshake one read can carry several frames; every header is still checked.
    FrameDecoder many(max_frame);
    pms_check(many.feed(two) && many.next().value().size() == 16 && many.next().value().size() == 512 && !many.next(), "frames split in order");
    two.push_back(std::byte{0x00});
    two.push_back(std::byte{0x0F});
    FrameDecoder later(max_frame);
    pms_check(!later.feed(two) && later.failed() && !later.next(), "a short frame later in a read fails");
}
