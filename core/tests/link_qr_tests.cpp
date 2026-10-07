#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pms/link/qr.hpp"
#include "test_harness.hpp"

namespace {

// Every payload byte equals its offset (the version byte at offset 0 is 1), so a misplaced field shows at once.
pms::link::PairingCode counting_code() {
    pms::link::PairingCode c;
    std::uint8_t next = 1;
    for (auto& b : c.mac) b = next++;
    for (auto& b : c.service_uuid) b = next++;
    for (auto& b : c.desktop_key) b = next++;
    for (auto& b : c.token) b = next++;
    c.expiry_unix_s = 0x4748494A;  // bytes 71..74
    return c;
}

// The test vector in docs/PROTOCOL.md section 3, computed independently with Python's base64.b32encode.
constexpr std::string_view counting_text =
    "PMS1:AEAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFYYDENBWHA5DYPSAIJCEMSCKJRHFAUSUKZMFUXC6MBRGIZTINJWG44DSOR3HQ6T4P2AIFBEGRCFIZDUQSKK";

bool in_alphabet(const char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '7'); }

std::vector<std::uint8_t> bytes_of(const std::string_view text) {
    std::vector<std::uint8_t> out;
    for (const char ch : text) out.push_back(static_cast<std::uint8_t>(ch));
    return out;
}

void check_base32() {
    using namespace pms::link;
    using namespace std::string_view_literals;
    // RFC 4648 section 10 vectors, without the padding.
    const std::array<std::pair<std::string_view, std::string_view>, 7> vectors{{
        {"", ""}, {"f", "MY"}, {"fo", "MZXQ"}, {"foo", "MZXW6"}, {"foob", "MZXW6YQ"}, {"fooba", "MZXW6YTB"}, {"foobar", "MZXW6YTBOI"}}};
    for (const auto& [plain, encoded] : vectors) {
        pms_check(base32_encode(bytes_of(plain)) == encoded && base32_decode(encoded) == bytes_of(plain), "RFC 4648 vectors both ways");
    }
    pms_check(!base32_decode("mzxw6ytboi"), "lowercase refused");
    pms_check(!base32_decode("MZXW6YTBOI======") && !base32_decode("MY======"), "padding refused");
    pms_check(!base32_decode("MZXW6YTBOJ") && !base32_decode("MZ"), "non-zero leftover bits refused: only canonical text decodes");
    pms_check(!base32_decode("M") && !base32_decode("MZX") && !base32_decode("MZXW6Y"), "lengths of 1, 3 and 6 mod 8 refused");
    pms_check(!base32_decode("A") && !base32_decode("MYA") && !base32_decode("MZXW6A") && !base32_decode("MZXW6YTBA"),
              "a surplus character is refused even when its bits are zero");
    pms_check(!base32_decode(" MZXW6YTBOI") && !base32_decode("MZXW6YTBOI\n") && !base32_decode("MZ\0Q"sv),
              "whitespace and NUL refused");

    // Every length from 0 to 256 bytes covers every tail size and every byte value.
    std::vector<std::uint8_t> bytes;
    for (std::size_t n = 0; n <= 256; ++n) {
        const auto text = base32_encode(bytes);
        pms_check(text.size() == (n * 8 + 4) / 5 && base32_decode(text) == bytes, "round trip at every length");
        bytes.push_back(static_cast<std::uint8_t>(n * 37 + 11));
    }
}

void check_pairing_vector() {
    using namespace pms::link;
    const auto code = counting_code();
    const auto text = encode_pairing(code);
    pms_check(text == counting_text, "encoder matches the PROTOCOL.md test vector");
    std::vector<std::uint8_t> payload{1};
    for (std::uint8_t offset = 1; offset < 75; ++offset) payload.push_back(offset);
    pms_check(text == "PMS1:" + base32_encode(payload), "version, MAC, UUID, key, token, then the expiry big-endian");
    const auto back = decode_pairing(counting_text, 0);
    pms_check(back && *back == code, "the test vector decodes to every field");
    pms_check(decode_pairing(text, code.expiry_unix_s) && !decode_pairing(text, code.expiry_unix_s + 1), "valid through the expiry second");
}

void check_pairing_sweep() {
    using namespace pms::link;
    using namespace std::string_view_literals;
    const auto code = counting_code();
    const auto text = encode_pairing(code);
    for (std::size_t n = 0; n < text.size(); ++n) {
        pms_check(!decode_pairing(std::string_view(text).substr(0, n), 0), "every prefix refused");
    }
    for (const auto extra : {"A"sv, "AA"sv, "AAAAAAAA"sv, "="sv, " "sv, "\n"sv, "\0"sv}) {
        pms_check(!decode_pairing(text + std::string(extra), 0), "trailing data refused");
    }
    pms_check(!decode_pairing(" " + text, 0) && !decode_pairing("pms1:" + text.substr(5), 0), "leading data and a lowercase prefix refused");

    // Every single-character substitution with every byte value. There is no checksum, so an in-alphabet change of a
    // field character is a different valid code; the rule is that decode only accepts text it would encode itself.
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        for (int value = 0; value < 256; ++value) {
            const auto ch = static_cast<char>(value);
            if (ch == text[i]) continue;
            auto mutated = text;
            mutated[i] = ch;
            const auto decoded = decode_pairing(mutated, 0);
            if (!decoded) continue;
            ++accepted;
            pms_check(i >= 6 && in_alphabet(ch), "a changed prefix, version character or non-alphabet character is refused");
            pms_check(encode_pairing(*decoded) == mutated, "a decoded substitution re-encodes to exactly its own text");
        }
    }
    // Character 6 carries the version's low 3 bits and the MAC's top 2 ('E' = 00100): only E..H keep version 1.
    // Characters 7..124 each carry field bits only, so all 31 other alphabet characters decode.
    pms_check(accepted == 3 + 118 * 31, "every substitution that keeps version 1 in the alphabet decodes");
}

}  // namespace

void run_link_qr_tests() {
    using namespace pms::link;
    const std::vector<std::uint8_t> foo{'f', 'o', 'o', 'b', 'a', 'r'};
    pms_check(base32_encode(foo) == "MZXW6YTBOI", "RFC 4648 vector foobar");
    pms_check(base32_decode("MZXW6YTBOI") == foo, "decode vector");
    pms_check(!base32_decode("MZXW6YTB0I"), "digit zero is not Base32");
    PairingCode c; c.mac = {1, 2, 3, 4, 5, 6}; c.token[0] = 9; c.desktop_key[31] = 0xAB; c.expiry_unix_s = 1000;
    const auto text = encode_pairing(c);
    pms_check(text.size() == 125 && text.starts_with("PMS1:"), "prefix + 120 Base32 chars");
    for (char ch : text) pms_check((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '7') || ch == ':' || ch == 'P' || ch == 'M' || ch == 'S' || ch == '1', "QR alphanumeric charset only");
    const auto back = decode_pairing(text, 999);
    pms_check(back && back->mac == c.mac && back->desktop_key == c.desktop_key && back->token == c.token, "round trip");
    pms_check(!decode_pairing(text, 1001), "expired codes are refused");
    pms_check(!decode_pairing("PMS2:" + text.substr(5), 999), "unknown prefix refused");
    pms_check(!decode_pairing(text.substr(0, 60), 999), "truncated code refused");

    check_base32();
    check_pairing_vector();
    check_pairing_sweep();
}
