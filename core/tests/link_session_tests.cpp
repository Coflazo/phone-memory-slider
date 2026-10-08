#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <noise/protocol.h>  // forges message 1 for a small-order phone key, step by step

#include "link/noise_ik.hpp"  // internal: NoiseIk drives the cacophony vectors and hand-built messages
#include "pms/link/frames.hpp"
#include "pms/link/messages.hpp"
#include "pms/link/session.hpp"
#include "test_harness.hpp"

#ifndef PMS_TEST_FIXTURES
#error "PMS_TEST_FIXTURES must point at core/tests/fixtures"
#endif
#ifndef PMS_LINK_TEST_HOOKS
#error "pms_core_tests must link pms_link_testing, the session library built with the fixed-ephemeral hook"
#endif

// R15: the hook build's symbols are in pms::link::testing_hooks, so they cannot collide with pms_link's. These name the
// namespace explicitly and fail to compile if the declarations are anywhere else.
static_assert(std::is_same_v<pms::link::ResponderSession, pms::link::testing_hooks::ResponderSession>);
static_assert(std::is_same_v<pms::link::InitiatorSession, pms::link::testing_hooks::InitiatorSession>);
static_assert(std::is_same_v<pms::link::PairingWindow, pms::link::testing_hooks::PairingWindow>);
static_assert(std::is_same_v<pms::link::detail::NoiseIk, pms::link::testing_hooks::detail::NoiseIk>);
static_assert(&pms::link::generate_keypair == &pms::link::testing_hooks::generate_keypair);
static_assert(&pms::link::set_fixed_ephemeral_for_tests == &pms::link::testing_hooks::set_fixed_ephemeral_for_tests);

namespace {
struct MemoryPins : pms::link::PinStore {
    std::map<pms::link::Key32, pms::link::Pinned> m;
    std::optional<pms::link::Pinned> find(const pms::link::Key32& k) const override { auto it = m.find(k); return it == m.end() ? std::nullopt : std::optional(it->second); }
    void pin(const pms::link::Pinned& p) override { m[p.phone_key] = p; }
    void unpin(const pms::link::Key32& k) override { m.erase(k); }
};

struct FailingPins : MemoryPins {
    void pin(const pms::link::Pinned&) override { throw std::runtime_error("pin store unavailable"); }
};

using Bytes = std::vector<std::byte>;
using pms::link::Key32;
using pms::link::KeyPair;
using pms::link::detail::NoiseIk;
using pms::link::detail::Role;

void noise_ok(const int err) { pms_check(err == NOISE_ERROR_NONE, "noise-c call in a test helper"); }

Key32 x25519(const KeyPair& mine, const Key32& theirs) {
    NoiseDHState* a = nullptr;
    NoiseDHState* b = nullptr;
    noise_ok(noise_dhstate_new_by_id(&a, NOISE_DH_CURVE25519));
    noise_ok(noise_dhstate_new_by_id(&b, NOISE_DH_CURVE25519));
    noise_ok(noise_dhstate_set_keypair(a, mine.private_key.data(), 32, mine.public_key.data(), 32));
    noise_ok(noise_dhstate_set_public_key(b, theirs.data(), 32));
    Key32 out{};
    noise_ok(noise_dhstate_calculate(a, b, out.data(), out.size()));
    noise_dhstate_free(a);
    noise_dhstate_free(b);
    return out;
}

// Message 1 (-> e, es, s, ss, payload) built token by token from the Noise spec with noise-c's SymmetricState, for a
// phone that claims static key `claimed` and uses `ss` as DH(s, rs). With a small-order key ss is all zeros, so
// anyone can build it: no private key is involved.
Bytes forged_message1(const Key32& claimed, const Key32& ss, const Bytes& payload, const Key32& desktop_public,
                      const std::span<const std::uint8_t> prologue) {
    NoiseSymmetricState* raw_state = nullptr;
    noise_ok(noise_symmetricstate_new_by_name(&raw_state, "Noise_IK_25519_ChaChaPoly_SHA256"));
    const std::unique_ptr<NoiseSymmetricState, decltype(&noise_symmetricstate_free)> sym(raw_state, &noise_symmetricstate_free);
    const auto encrypt_and_hash = [&](const Bytes& plain) {
        std::vector<std::uint8_t> buffer(plain.size() + 16);
        std::transform(plain.begin(), plain.end(), buffer.begin(), [](const std::byte b) { return static_cast<std::uint8_t>(b); });
        NoiseBuffer inout;
        noise_buffer_set_inout(inout, buffer.data(), plain.size(), buffer.size());
        noise_ok(noise_symmetricstate_encrypt_and_hash(sym.get(), &inout));
        buffer.resize(inout.size);
        return buffer;
    };
    noise_ok(noise_symmetricstate_mix_hash(sym.get(), prologue.data(), prologue.size()));
    noise_ok(noise_symmetricstate_mix_hash(sym.get(), desktop_public.data(), desktop_public.size()));  // <- s
    const KeyPair e = pms::link::generate_keypair();
    noise_ok(noise_symmetricstate_mix_hash(sym.get(), e.public_key.data(), e.public_key.size()));  // e
    const Key32 es = x25519(e, desktop_public);
    noise_ok(noise_symmetricstate_mix_key(sym.get(), es.data(), es.size()));  // es
    Bytes claimed_bytes;
    for (const auto b : claimed) {
        claimed_bytes.push_back(static_cast<std::byte>(b));
    }
    const auto s = encrypt_and_hash(claimed_bytes);                          // s
    noise_ok(noise_symmetricstate_mix_key(sym.get(), ss.data(), ss.size()));  // ss
    const auto body = encrypt_and_hash(payload);
    Bytes out;
    for (const auto part : {std::span<const std::uint8_t>(e.public_key), std::span<const std::uint8_t>(s),
                            std::span<const std::uint8_t>(body)}) {
        for (const auto b : part) {
            out.push_back(static_cast<std::byte>(b));
        }
    }
    return out;
}

Bytes ascii(const std::string_view text) {
    Bytes out;
    for (const char ch : text) {
        out.push_back(static_cast<std::byte>(ch));
    }
    return out;
}

template <typename Range>
Bytes as_bytes(const Range& range) {
    Bytes out;
    for (const auto value : range) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// "PMS/1" + the 16 service UUID bytes, built here independently of the session.
std::vector<std::uint8_t> prologue_for(const std::array<std::uint8_t, 16>& uuid) {
    std::vector<std::uint8_t> out{'P', 'M', 'S', '/', '1'};
    out.insert(out.end(), uuid.begin(), uuid.end());
    return out;
}

// u8 kind | token (kind 1) | u16 BE length | model, built here independently of the session.
Bytes payload_for(const std::uint8_t kind, const std::array<std::uint8_t, 16>* token, const std::string_view model) {
    Bytes out{static_cast<std::byte>(kind)};
    if (token != nullptr) {
        const auto t = as_bytes(*token);
        out.insert(out.end(), t.begin(), t.end());
    }
    out.push_back(static_cast<std::byte>(model.size() >> 8U));
    out.push_back(static_cast<std::byte>(model.size() & 0xFFU));
    const auto m = ascii(model);
    out.insert(out.end(), m.begin(), m.end());
    return out;
}

KeyPair pair_of(const Key32& private_key) { return KeyPair{pms::link::detail::x25519_public_key(private_key), private_key}; }

std::vector<std::uint8_t> from_hex(const std::string_view hex) {
    pms_check(hex.size() % 2 == 0, "hex has an even length");
    const auto nibble = [](const char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(ch - 'a' + 10);
        pms_check(false, "hex digits are 0-9a-f");
        return 0;
    };
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>((nibble(hex[i]) << 4U) | nibble(hex[i + 1])));
    }
    return out;
}

struct CacophonyVector {
    std::string protocol_name;
    std::map<std::string, std::vector<std::uint8_t>> fields;  // init_static, resp_ephemeral, handshake_hash, ...
    std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> messages;  // payload, ciphertext
};

// The fixture keeps cacophony's pretty-printed JSON verbatim: one "key": "value" pair per line, after # comments.
std::vector<CacophonyVector> parse_cacophony(const std::string& path) {
    std::ifstream in(path);
    pms_check(in.good(), "cacophony_ik.txt opens");
    std::vector<CacophonyVector> out;
    std::optional<std::vector<std::uint8_t>> payload;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find("\": \"");
        if (line.starts_with('#') || !line.starts_with('"') || colon == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(1, colon - 1);
        const auto start = colon + 4;
        const auto end = line.find('"', start);
        pms_check(end != std::string::npos, "cacophony value is quoted");
        const std::string value = line.substr(start, end - start);
        if (key == "protocol_name") {
            out.push_back(CacophonyVector{value, {}, {}});
            continue;
        }
        pms_check(!out.empty(), "cacophony fields follow a protocol_name");
        if (key == "payload") {
            payload = from_hex(value);
        } else if (key == "ciphertext") {
            pms_check(payload.has_value(), "cacophony ciphertext follows its payload");
            out.back().messages.emplace_back(*payload, from_hex(value));
            payload.reset();
        } else {
            out.back().fields[key] = from_hex(value);
        }
    }
    return out;
}

// Runs each vector through NoiseIk, the code both sessions use, with the vector's keys set through the same hook.
int run_cacophony_ik_vectors(const std::string& path) {
    int checked = 0;
    for (const auto& v : parse_cacophony(path)) {
        pms_check(v.protocol_name == "Noise_IK_25519_ChaChaPoly_SHA256", "the fixture holds only our exact pattern");
        const auto field = [&](const char* name) {
            const auto it = v.fields.find(name);
            pms_check(it != v.fields.end(), name);
            return it->second;
        };
        const auto key = [&](const char* name) {
            const auto bytes = field(name);
            pms_check(bytes.size() == 32, name);
            Key32 k{};
            std::copy(bytes.begin(), bytes.end(), k.begin());
            return k;
        };
        const auto prologue = field("init_prologue");
        pms_check(prologue == field("resp_prologue"), "one prologue");
        const Key32 remote = key("init_remote_static");
        pms_check(pair_of(key("resp_static")).public_key == remote, "init_remote_static is resp_static's public key");

        pms::link::set_fixed_ephemeral_for_tests(pair_of(key("resp_ephemeral")), pair_of(key("init_ephemeral")));
        NoiseIk init(Role::Initiator, pair_of(key("init_static")), prologue, &remote);
        NoiseIk resp(Role::Responder, pair_of(key("resp_static")), prologue, nullptr);
        pms::link::set_fixed_ephemeral_for_tests(std::nullopt, std::nullopt);

        pms_check(v.messages.size() > 2, "vector has handshake and transport messages");
        for (std::size_t n = 0; n < v.messages.size(); ++n) {
            // Cacophony alternates from the initiator, through the handshake and after it.
            NoiseIk& sender = n % 2 == 0 ? init : resp;
            NoiseIk& receiver = n % 2 == 0 ? resp : init;
            const auto payload = as_bytes(v.messages[n].first);
            const auto sent = n < 2 ? sender.write_message(payload) : sender.seal(payload);
            pms_check(sent && *sent == as_bytes(v.messages[n].second), "cacophony ciphertext matches byte for byte");
            const auto got = n < 2 ? receiver.read_message(*sent) : receiver.open(*sent);
            pms_check(got && *got == payload, "cacophony payload decrypts");
            if (n == 1) {
                pms_check(init.established() && resp.established(), "IK completes in two messages");
                const auto hash = field("handshake_hash");
                pms_check(std::equal(hash.begin(), hash.end(), init.handshake_hash().begin(), init.handshake_hash().end()) &&
                              init.handshake_hash() == resp.handshake_hash(),
                          "cacophony handshake hash matches");
                pms_check(resp.remote_static() == pair_of(key("init_static")).public_key, "responder learns the initiator key");
            }
        }
        ++checked;
    }
    return checked;
}

// Fixed X25519 private keys for the golden transcript: bytes first, first + 1, ..., clamped as RFC 7748 does.
Key32 counting_key(const std::uint8_t first) {
    Key32 k{};
    for (std::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<std::uint8_t>(first + i);
    }
    k[0] &= 248U;
    k[31] = static_cast<std::uint8_t>((k[31] & 127U) | 64U);
    return k;
}

void put_field(Bytes& out, const Bytes& field) {
    out.push_back(static_cast<std::byte>(field.size() >> 8U));
    out.push_back(static_cast<std::byte>(field.size() & 0xFFU));
    out.insert(out.end(), field.begin(), field.end());
}

// golden_ik_transcript.bin: 22 fields, each u16 BE length + bytes; the order is in docs/PROTOCOL.md section 11.
// Android's NoiseSession test (noise-java) must reproduce it byte for byte; change it only with a protocol change.
Bytes golden_ik_transcript() {
    using namespace pms::link;
    const KeyPair desk = pair_of(counting_key(0x00));
    const KeyPair desk_ephemeral = pair_of(counting_key(0x20));
    const KeyPair phone = pair_of(counting_key(0x40));
    const KeyPair phone_ephemeral = pair_of(counting_key(0x60));
    std::array<std::uint8_t, 16> token{};
    for (std::size_t i = 0; i < token.size(); ++i) {
        token[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    // 8f4d9d6a-0c84-4a7f-a36a-6fc12c4b34bf, the companion's RFCOMM service UUID.
    const std::array<std::uint8_t, 16> uuid{0x8f, 0x4d, 0x9d, 0x6a, 0x0c, 0x84, 0x4a, 0x7f,
                                            0xa3, 0x6a, 0x6f, 0xc1, 0x2c, 0x4b, 0x34, 0xbf};
    const std::string_view model = "Pixel 9";

    set_fixed_ephemeral_for_tests(desk_ephemeral, phone_ephemeral);
    MemoryPins pins;
    ResponderSession desktop(desk, uuid, pins);
    InitiatorSession phone_side(phone, desk.public_key, uuid);
    PairingWindow window(token, 1000);
    desktop.open_pairing_window(window);
    const auto msg1 = phone_side.message1_first_pairing(token, model);
    Bytes msg2;
    const bool done = desktop.on_message1(msg1, 1000, msg2) == HandshakeResult::Established && phone_side.on_message2(msg2);
    set_fixed_ephemeral_for_tests(std::nullopt, std::nullopt);
    pms_check(done && phone_side.sas() == desktop.sas(), "golden handshake completes with one code");

    Bytes out;
    for (const auto& field : {as_bytes(desk.private_key), as_bytes(desk.public_key), as_bytes(desk_ephemeral.private_key),
                              as_bytes(phone.private_key), as_bytes(phone.public_key), as_bytes(phone_ephemeral.private_key),
                              as_bytes(uuid), as_bytes(prologue_for(uuid)), as_bytes(token), ascii(model),
                              payload_for(1, &token, model), msg1, msg2, ascii(desktop.sas())}) {
        put_field(out, field);
    }
    // Transport alternates phone, desktop, phone, desktop; the last plaintext is empty (a bare 16-byte tag).
    for (const auto& text : {std::string_view("phone 1"), std::string_view("desktop 1"), std::string_view("phone 2"),
                             std::string_view("")}) {
        const bool from_phone = text.starts_with("phone");
        const auto ct = from_phone ? phone_side.seal(ascii(text)) : desktop.seal(ascii(text));
        const auto back = from_phone ? desktop.open(ct) : phone_side.open(ct);
        pms_check(back && *back == ascii(text), "golden transport round trip");
        put_field(out, ascii(text));
        put_field(out, ct);
    }
    return out;
}

void check_golden_ik_transcript() {
    const std::filesystem::path path = std::filesystem::path(PMS_TEST_FIXTURES) / "golden_ik_transcript.bin";
    pms_check(std::filesystem::exists(path), "golden_ik_transcript.bin exists (pms_core_tests --write-golden core/tests/fixtures)");
    std::ifstream in(path, std::ios::binary);
    Bytes file;
    for (auto it = std::istreambuf_iterator<char>(in); it != std::istreambuf_iterator<char>(); ++it) {
        file.push_back(static_cast<std::byte>(*it));
    }
    const auto generated = golden_ik_transcript();
    pms_check(file == generated, "session output matches golden_ik_transcript.bin byte for byte");
    pms_check(golden_ik_transcript() == generated, "the golden transcript is deterministic");
}

template <typename F>
bool throws_as(F&& f, const std::string_view kind) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return kind == "invalid_argument";
    } catch (const std::length_error&) {
        return kind == "length_error";
    } catch (const std::logic_error&) {
        return kind == "logic_error";
    }
    return false;
}

}  // namespace

void run_link_session_tests() {
    using namespace pms::link;
    // 1. cacophony vectors for the exact pattern (parse init/resp static+ephemeral, prologue, messages; compare ciphertexts).
    pms_check(run_cacophony_ik_vectors(std::string(PMS_TEST_FIXTURES) + "/cacophony_ik.txt") >= 1, "cacophony IK vectors pass");
    const std::array<std::uint8_t, 16> uuid{1, 2, 3};
    const auto desk = generate_keypair(), phone = generate_keypair(), thief = generate_keypair();
    MemoryPins pins;
    ResponderSession r(desk, uuid, pins);
    const std::array<std::uint8_t, 16> token{42};
    PairingWindow window(token, 1000);  // R11: one window per QR code, shared by every connection's session
    r.open_pairing_window(window);
    InitiatorSession i(phone, desk.public_key, uuid);
    std::vector<std::byte> m2;
    pms_check(r.on_message1(i.message1_first_pairing(token, "Pixel 9"), 900, m2) == HandshakeResult::Established, "first pairing with the QR token");
    pms_check(i.on_message2(m2) && i.sas() == r.sas() && r.sas().size() == 6, "both sides derive the same 6-digit code");
    pms_check(pins.find(phone.public_key).has_value(), "phone key pinned");
    pms_check(window.burned(), "the pairing burned the window");
    // Review Focus 1: the token is single use.
    ResponderSession r2(desk, uuid, pins);
    r2.open_pairing_window(window);  // the same QR code, now burned
    InitiatorSession t(thief, desk.public_key, uuid);
    pms_check(r2.on_message1(t.message1_first_pairing(token, "Thief"), 901, m2) == HandshakeResult::Rejected, "a reused token is rejected");
    pms_check(!pins.find(thief.public_key).has_value(), "the second phone is not pinned");
    // Expired window.
    ResponderSession r3(desk, uuid, pins);
    PairingWindow fresh(token, 1000);
    r3.open_pairing_window(fresh);
    pms_check(r3.on_message1(InitiatorSession(thief, desk.public_key, uuid).message1_first_pairing(token, "x"), 1001, m2) == HandshakeResult::Rejected, "expired window rejects");
    pms_check(!fresh.burned(), "an expired attempt does not burn the window");
    // Review Focus 2: reconnect with the pin, no token.
    ResponderSession r4(desk, uuid, pins);
    InitiatorSession again(phone, desk.public_key, uuid);
    pms_check(r4.on_message1(again.message1_reconnect("Pixel 9"), 5000, m2) == HandshakeResult::Established && again.on_message2(m2), "pinned phone reconnects");
    ResponderSession r5(desk, uuid, pins);
    pms_check(r5.on_message1(InitiatorSession(thief, desk.public_key, uuid).message1_reconnect("x"), 5000, m2) == HandshakeResult::Rejected, "unpinned reconnect rejected");
    // Wrong desktop key (phone scanned someone else's QR) fails closed.
    InitiatorSession wrong(phone, thief.public_key, uuid);
    ResponderSession r6(desk, uuid, pins);
    pms_check(r6.on_message1(wrong.message1_reconnect("x"), 5000, m2) == HandshakeResult::Rejected, "msg1 for another desktop is rejected");
    // Transport: tamper closes.
    auto ct = again.seal(std::vector<std::byte>{std::byte{1}, std::byte{2}});
    auto ok = r4.open(ct);
    pms_check(ok && ok->size() == 2, "transport round trip");
    auto ct2 = again.seal(std::vector<std::byte>{std::byte{3}});
    ct2[0] ^= std::byte{1};
    pms_check(!r4.open(ct2), "tampered ciphertext closes the session");
    // Garbage msg1 is rejected without crashing.
    std::vector<std::byte> junk(200, std::byte{0x5A});
    ResponderSession r7(desk, uuid, pins);
    pms_check(r7.on_message1(junk, 5000, m2) == HandshakeResult::Rejected, "garbage msg1 rejected");

    // --- Beyond the brief ---
    pms_check(desk.public_key != phone.public_key && phone.public_key != thief.public_key && desk.private_key != phone.private_key,
              "generate_keypair draws fresh keys");
    pms_check(m2.empty(), "a rejected msg1 leaves msg2_out empty");

    // Who paired, and how.
    pms_check(r.first_pairing() && r.peer().phone_key == phone.public_key && r.peer().model == "Pixel 9" && r.peer().paired_unix_s == 900,
              "kind 1 pins the phone key, model and pairing time");
    pms_check(!r4.first_pairing() && r4.peer().model == "Pixel 9" && r4.peer().paired_unix_s == 900, "kind 2 uses the stored pin");
    pms_check(throws_as([&] { (void)r7.peer(); }, "logic_error") && throws_as([&] { (void)r7.sas(); }, "logic_error"),
              "peer and code exist only after Established");

    // The window is valid through its expiry second, needs the exact token, and one handshake per session.
    {
        ResponderSession edge(desk, uuid, pins);
        PairingWindow edge_window(token, 1000);
        edge.open_pairing_window(edge_window);
        InitiatorSession p(phone, desk.public_key, uuid);
        pms_check(edge.on_message1(p.message1_first_pairing(token, "Pixel 9"), 1000, m2) == HandshakeResult::Established,
                  "now == expiry is still inside the window");
        pms_check(edge.on_message1(InitiatorSession(phone, desk.public_key, uuid).message1_reconnect("Pixel 9"), 1000, m2) ==
                      HandshakeResult::Rejected,
                  "a second msg1 on one session is rejected");
        auto other_token = token;
        other_token[15] ^= 1U;
        ResponderSession wrong_token(desk, uuid, pins);
        PairingWindow wrong_token_window(token, 1000);
        wrong_token.open_pairing_window(wrong_token_window);
        pms_check(wrong_token.on_message1(InitiatorSession(thief, desk.public_key, uuid).message1_first_pairing(other_token, "x"), 900, m2) ==
                      HandshakeResult::Rejected,
                  "a token differing in one bit is rejected");
        ResponderSession no_window(desk, uuid, pins);
        pms_check(no_window.on_message1(InitiatorSession(phone, desk.public_key, uuid).message1_first_pairing(token, "x"), 900, m2) ==
                      HandshakeResult::Rejected,
                  "kind 1 needs an open window even for a pinned phone");
    }

    // Message 1's payload, read with a bare NoiseIk responder; the code comes from the handshake hash.
    const auto prologue = prologue_for(uuid);
    {
        InitiatorSession p(phone, desk.public_key, uuid);
        NoiseIk d(Role::Responder, desk, prologue, nullptr);
        const auto payload = d.read_message(p.message1_first_pairing(token, "Pixel 9"));
        pms_check(payload && *payload == payload_for(1, &token, "Pixel 9"), "kind 1 payload: kind | token | string model");
        pms_check(d.remote_static() == phone.public_key, "msg1 carries the phone's static key");
        const auto reply = d.write_message({});
        pms_check(reply && reply->size() == 48 && p.on_message2(*reply), "message 2 is e plus a tag over an empty payload");
        pms_check(p.sas() == detail::sas_digits(d.handshake_hash()), "the code comes from the handshake hash");

        InitiatorSession q(phone, desk.public_key, uuid);
        NoiseIk d2(Role::Responder, desk, prologue, nullptr);
        const auto payload2 = d2.read_message(q.message1_reconnect("Pixel 9"));
        pms_check(payload2 && *payload2 == payload_for(2, nullptr, "Pixel 9"), "kind 2 payload: kind | string model");
        const auto bad_reply = d2.write_message(std::vector<std::byte>{std::byte{0}});
        pms_check(bad_reply && !q.on_message2(*bad_reply), "a message 2 with a payload fails the phone");
        pms_check(throws_as([&] { (void)q.sas(); }, "logic_error") && throws_as([&] { (void)q.seal({}); }, "logic_error"),
                  "a failed phone session has no code and no transport");
    }

    // The responder refuses every malformed payload (sent by a bare NoiseIk initiator).
    pins.pin(Pinned{thief.public_key, "Thief", 1});
    const auto send_raw = [&](const Bytes& payload) {
        NoiseIk p(Role::Initiator, thief, prologue, &desk.public_key);
        const auto msg1 = p.write_message(payload);
        pms_check(msg1.has_value(), "raw msg1 written");
        ResponderSession s(desk, uuid, pins);
        PairingWindow raw_window(token, 1000);
        s.open_pairing_window(raw_window);
        return s.on_message1(*msg1, 900, m2);
    };
    pms_check(send_raw(payload_for(2, nullptr, "ok")) == HandshakeResult::Established, "the raw sender works");
    pms_check(send_raw(payload_for(1, &token, "ok")) == HandshakeResult::Established, "the raw sender pairs");
    const auto bad_utf8 = concat({Bytes{std::byte{2}, std::byte{0}, std::byte{2}}, Bytes{std::byte{0xC0}, std::byte{0x80}}});
    for (const auto& [payload, what] : std::vector<std::pair<Bytes, const char*>>{
             {Bytes{}, "empty payload"},
             {Bytes{std::byte{0}, std::byte{0}, std::byte{0}}, "kind 0"},
             {Bytes{std::byte{3}, std::byte{0}, std::byte{0}}, "kind 3"},
             {Bytes(1 + 10, std::byte{1}), "kind 1 with a short token"},
             {Bytes{std::byte{2}, std::byte{0}}, "a truncated length"},
             {Bytes{std::byte{2}, std::byte{0}, std::byte{5}, std::byte{'a'}}, "a length past the end"},
             {concat({payload_for(2, nullptr, "ok"), Bytes{std::byte{0}}}), "a trailing byte"},
             {bad_utf8, "an overlong UTF-8 model"},
             {concat({payload_for(1, &token, "ok"), Bytes{std::byte{0}}}), "a trailing byte after a token"}}) {
        pms_check(send_raw(payload) == HandshakeResult::Rejected, what);
    }
    pins.unpin(thief.public_key);

    // R11: one PairingWindow per QR code, shared by every connection's session. Only an Established kind 1 burns it.
    {
        const std::array<std::uint8_t, 16> code{7, 7, 7};
        auto wrong_code = code;
        wrong_code[0] ^= 0x80U;
        PairingWindow shared(code, 1000);
        const auto newcomer = generate_keypair(), second = generate_keypair();
        const auto try_pair = [&](const KeyPair& who, const std::array<std::uint8_t, 16>& presented, const std::uint32_t now,
                                  PinStore& store) {
            ResponderSession s(desk, uuid, store);
            s.open_pairing_window(shared);
            return s.on_message1(InitiatorSession(who, desk.public_key, uuid).message1_first_pairing(presented, "Pixel 9"), now, m2);
        };

        // (b) A wrong token on a session in between does not burn it.
        pms_check(try_pair(newcomer, wrong_code, 900, pins) == HandshakeResult::Rejected && !shared.burned(),
                  "a wrong token is rejected and leaves the window open");
        // (c) A reconnect while the window is open leaves it open and usable.
        {
            ResponderSession s(desk, uuid, pins);
            s.open_pairing_window(shared);
            pms_check(s.on_message1(InitiatorSession(phone, desk.public_key, uuid).message1_reconnect("Pixel 9"), 900, m2) ==
                              HandshakeResult::Established &&
                          !s.first_pairing() && !shared.burned(),
                      "a reconnect does not touch the window");
        }
        // (d) Only Established burns: not an expired attempt, a malformed payload, a small-order key or a failed pin.
        pms_check(try_pair(newcomer, code, 1001, pins) == HandshakeResult::Rejected && !shared.burned(),
                  "an expired attempt does not burn the window");
        {
            NoiseIk raw_phone(Role::Initiator, newcomer, prologue, &desk.public_key);
            const auto malformed = raw_phone.write_message(concat({payload_for(1, &code, "x"), Bytes{std::byte{0}}}));
            ResponderSession s(desk, uuid, pins);
            s.open_pairing_window(shared);
            pms_check(malformed && s.on_message1(*malformed, 900, m2) == HandshakeResult::Rejected && !shared.burned(),
                      "a malformed payload with the right token does not burn the window");
        }
        {
            ResponderSession s(desk, uuid, pins);
            s.open_pairing_window(shared);
            const Key32 zero{};
            pms_check(s.on_message1(forged_message1(zero, zero, payload_for(1, &code, "x"), desk.public_key, prologue), 900, m2) ==
                              HandshakeResult::Rejected &&
                          !shared.burned(),
                      "a small-order key with the right token does not burn the window");
        }
        {
            FailingPins failing;
            bool threw = false;
            try {
                (void)try_pair(newcomer, code, 900, failing);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            pms_check(threw && !shared.burned(), "a pin store failure propagates and leaves the window open");
        }
        // (a) The first phone pairs and burns it; a second phone with the same code on a new session is refused.
        pms_check(try_pair(newcomer, code, 1000, pins) == HandshakeResult::Established && shared.burned() &&
                      pins.find(newcomer.public_key).has_value(),
                  "the first phone with the code pairs and burns the window");
        pms_check(try_pair(second, code, 1000, pins) == HandshakeResult::Rejected && !pins.find(second.public_key).has_value(),
                  "a second phone with the same code on a new session is refused");
        // burn() closes a window early, e.g. when the QR code is hidden.
        PairingWindow hidden(code, 1000);
        hidden.burn();
        ResponderSession s(desk, uuid, pins);
        s.open_pairing_window(hidden);
        pms_check(s.on_message1(InitiatorSession(second, desk.public_key, uuid).message1_first_pairing(code, "x"), 900, m2) ==
                      HandshakeResult::Rejected,
                  "a window closed with burn() refuses its code");
        pins.unpin(newcomer.public_key);
    }

    // R12: a phone key of small order is refused before any pin lookup or pin. The forged message is otherwise valid:
    // the same builder with a real key and its real ss is accepted.
    {
        const Key32 real_ss = x25519(phone, desk.public_key);
        ResponderSession control(desk, uuid, pins);
        pms_check(control.on_message1(forged_message1(phone.public_key, real_ss, payload_for(2, nullptr, "Pixel 9"), desk.public_key, prologue),
                                      5000, m2) == HandshakeResult::Established,
                  "the forging helper builds a valid message 1");
        const Key32 zero{};
        Key32 order8{};  // a point of order 8 (RFC 7748 / libsodium's blocklist)
        const auto order8_bytes = from_hex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800");
        std::copy(order8_bytes.begin(), order8_bytes.end(), order8.begin());
        for (const auto& [key, what] : {std::pair{zero, "the all-zero phone key is rejected"}, std::pair{order8, "an order-8 phone key is rejected"}}) {
            pins.pin(Pinned{key, "degenerate", 1});  // even a pinned one: the check comes before the lookup
            const std::array<std::uint8_t, 16> code{9};
            PairingWindow w(code, 1000);
            ResponderSession reconnect(desk, uuid, pins);
            ResponderSession pairing(desk, uuid, pins);
            pairing.open_pairing_window(w);
            pms_check(reconnect.on_message1(forged_message1(key, zero, payload_for(2, nullptr, "x"), desk.public_key, prologue), 5000, m2) ==
                              HandshakeResult::Rejected &&
                          pairing.on_message1(forged_message1(key, zero, payload_for(1, &code, "x"), desk.public_key, prologue), 900, m2) ==
                              HandshakeResult::Rejected &&
                          !w.burned(),
                      what);
            pins.unpin(key);
        }
        // Every small-order encoding (libsodium's blocklist, plus the top bit X25519 ignores) and no real key.
        for (const char* hex : {"0000000000000000000000000000000000000000000000000000000000000000",
                                "0100000000000000000000000000000000000000000000000000000000000000",
                                "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
                                "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
                                "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                                "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                                "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                                "0000000000000000000000000000000000000000000000000000000000000080",
                                "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880"}) {
            Key32 k{};
            const auto bytes = from_hex(hex);
            std::copy(bytes.begin(), bytes.end(), k.begin());
            pms_check(detail::small_order_public_key(k), hex);
        }
        pms_check(!detail::small_order_public_key(phone.public_key) && !detail::small_order_public_key(desk.public_key),
                  "real keys are not small order");
    }

    // Message 1 must fit one 512-byte pre-handshake frame: 96 bytes of Noise overhead plus the payload.
    {
        InitiatorSession a(phone, desk.public_key, uuid);
        pms_check(a.message1_first_pairing(token, std::string(397, 'm')).size() == max_preauth_frame, "kind 1 fits a 397-byte model");
        InitiatorSession b(phone, desk.public_key, uuid);
        pms_check(b.message1_reconnect(std::string(413, 'm')).size() == max_preauth_frame, "kind 2 fits a 413-byte model");
        pms_check(throws_as([&] { (void)InitiatorSession(phone, desk.public_key, uuid).message1_first_pairing(token, std::string(398, 'm')); },
                            "length_error"),
                  "a kind 1 msg1 over 512 bytes is refused");
        pms_check(throws_as([&] { (void)InitiatorSession(phone, desk.public_key, uuid).message1_reconnect(std::string(414, 'm')); },
                            "length_error"),
                  "a kind 2 msg1 over 512 bytes is refused");
        pms_check(throws_as([&] { (void)InitiatorSession(phone, desk.public_key, uuid).message1_reconnect(std::string(1025, 'm')); },
                            "invalid_argument"),
                  "a model over 1024 bytes is refused");
        pms_check(throws_as([&] { (void)InitiatorSession(phone, desk.public_key, uuid).message1_reconnect("\xC0\x80"); }, "invalid_argument"),
                  "a model that is not UTF-8 is refused");
        pms_check(throws_as([&] { (void)a.message1_reconnect("x"); }, "logic_error"), "one message 1 per session");

        NoiseIk big(Role::Initiator, phone, prologue, &desk.public_key);
        const auto oversize = big.write_message(payload_for(2, nullptr, std::string(414, 'm')));
        ResponderSession s(desk, uuid, pins);
        pms_check(oversize && oversize->size() == max_preauth_frame + 1 && s.on_message1(*oversize, 5000, m2) == HandshakeResult::Rejected,
                  "the desktop refuses a msg1 over 512 bytes");
    }

    // Transport both ways, size limits, replay and death after a failure.
    {
        ResponderSession d(desk, uuid, pins);
        InitiatorSession p(phone, desk.public_key, uuid);
        pms_check(d.on_message1(p.message1_reconnect("Pixel 9"), 5000, m2) == HandshakeResult::Established && p.on_message2(m2),
                  "transport session established");
        const auto down = d.seal(ascii("from the desktop"));
        const auto down_back = p.open(down);
        pms_check(down.size() == 16 + 16 && down_back && *down_back == ascii("from the desktop"), "desktop to phone round trip");
        const auto empty = p.seal({});
        const auto empty_back = d.open(empty);
        pms_check(empty.size() == detail::tag_size && empty_back && empty_back->empty(), "an empty message is a bare tag");
        const Bytes biggest(max_plaintext, std::byte{7});
        const auto big_ct = p.seal(biggest);
        const auto big_back = d.open(big_ct);
        pms_check(big_ct.size() == max_frame && big_back && *big_back == biggest, "the largest plaintext fills one frame");
        pms_check(throws_as([&] { (void)p.seal(Bytes(max_plaintext + 1)); }, "length_error"), "one byte more does not fit a frame");

        const auto once = p.seal(ascii("once"));
        pms_check(d.open(once).has_value() && !d.open(once).has_value(), "a replayed transport message closes the session");
        pms_check(!d.open(p.seal(ascii("after"))).has_value(), "a closed session opens nothing");
        pms_check(throws_as([&] { (void)d.seal(ascii("x")); }, "logic_error"), "a closed session seals nothing");
        pms_check(!p.open(Bytes(15)).has_value() && !p.open(down).has_value(), "short input closes the phone side too");
    }

    // Internal helpers.
    std::array<std::uint8_t, 32> hash{};
    hash[3] = 42;
    pms_check(detail::sas_digits(hash) == "000042", "the code is zero-padded to 6 digits");
    hash[0] = hash[1] = hash[2] = hash[3] = 0xFF;
    pms_check(detail::sas_digits(hash) == "967295", "the code is the big-endian u32 modulo 1,000,000");
    const std::array<std::uint8_t, 3> abc{1, 2, 3}, abd{1, 2, 4};
    pms_check(detail::equal_constant_time(abc, abc) && !detail::equal_constant_time(abc, abd) &&
                  !detail::equal_constant_time(abc, std::span<const std::uint8_t>(abc).first(2)),
              "constant-time compare");
    pms_check(throws_as([&] { NoiseIk(Role::Responder, KeyPair{phone.public_key, desk.private_key}, prologue, nullptr); }, "invalid_argument"),
              "a key pair whose halves do not match is refused");

    check_golden_ik_transcript();
}

void write_link_golden_ik_transcript(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    const auto bytes = golden_ik_transcript();
    std::ofstream out(dir / "golden_ik_transcript.bin", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    pms_check(out.good(), "golden_ik_transcript.bin written");
}
