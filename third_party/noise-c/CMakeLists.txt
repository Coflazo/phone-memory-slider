# Vendored noise-c (signalapp fork, MIT): only what Noise_IK_25519_ChaChaPoly_SHA256 needs. See README.md.
# noise_rand_bytes is not here: core/src/link/random.cpp provides it (OS CSPRNG) and core/CMakeLists.txt links it in.

# Vendored code keeps its own warnings: drop the project's /W4 /WX (inherited from the top level) for this directory.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")

add_library(noise_c STATIC
  src/protocol/cipherstate.c
  src/protocol/dhstate.c
  src/protocol/errors.c
  src/protocol/handshakestate.c
  src/protocol/hashstate.c
  src/protocol/internal.c
  src/protocol/names.c
  src/protocol/patterns.c
  src/protocol/randstate.c
  src/protocol/signstate.c
  src/protocol/symmetricstate.c
  src/protocol/util.c
  src/backend/ref/cipher-chachapoly.c
  src/backend/ref/dh-curve25519.c
  src/backend/ref/hash-sha256.c
  src/crypto/chacha/chacha.c
  src/crypto/donna/poly1305-donna.c
  src/crypto/sha2/sha256.c
  src/crypto/sha2/sha512.c
  src/crypto/ed25519/ed25519.c
  pms_unused_algorithms.c
)

target_include_directories(noise_c
  PUBLIC include
  PRIVATE src src/protocol
)
# Upstream's flags for the reference Ed25519 code (Curve25519 uses its fast base-point multiply):
# SHA-512 from src/crypto/sha2, randomness from noise_rand_bytes.
target_compile_definitions(noise_c PRIVATE ED25519_CUSTOMHASH ED25519_CUSTOMRANDOM)
if(MSVC)
  target_compile_options(noise_c PRIVATE /W0)
else()
  target_compile_options(noise_c PRIVATE -w)
endif()
