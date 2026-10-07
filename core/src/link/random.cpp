// Randomness for noise-c, which declares noise_rand_bytes and calls it for every key it generates. Linked into the
// noise_c library (core/CMakeLists.txt) in place of noise-c's own rand_os.c.

#include <cstddef>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#include <unistd.h>
#endif

// Fills bytes from the OS CSPRNG or aborts: a key from a failed RNG must never exist.
extern "C" void noise_rand_bytes(void* bytes, std::size_t size) {
    auto* out = static_cast<unsigned char*>(bytes);
#ifdef _WIN32
    constexpr std::size_t max_chunk = 1U << 20U;  // BCryptGenRandom takes a ULONG length
#else
    constexpr std::size_t max_chunk = 256;  // getentropy's limit per call
#endif
    while (size > 0) {
        const std::size_t chunk = size < max_chunk ? size : max_chunk;
#ifdef _WIN32
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, static_cast<ULONG>(chunk), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            std::abort();
        }
#else
        if (getentropy(out, chunk) != 0) {
            std::abort();
        }
#endif
        out += chunk;
        size -= chunk;
    }
}

// noise-c builds ed25519-donna with ED25519_CUSTOMRANDOM, which needs this for batch verification. Nothing here
// verifies signatures; it is defined so the library links, from the same source.
extern "C" void ed25519_randombytes_unsafe(void* bytes, std::size_t size) { noise_rand_bytes(bytes, size); }
