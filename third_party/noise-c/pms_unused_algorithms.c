/* Not part of noise-c: added by Phone Memory Slider (see README.md).

   The vendored subset builds Curve25519, ChaChaPoly and SHA-256 only (plus the SHA-512 and Ed25519 code that
   Curve25519 key derivation uses internally). noise-c's algorithm switches still name every other algorithm, so
   their constructors return NULL here: creating such a state fails with NOISE_ERROR_NO_MEMORY instead of the
   build failing to link. */

#include "internal.h"

NoiseCipherState *noise_aesgcm_new_ref(void) { return 0; } /* called by noise_aesgcm_new() in internal.c */
NoiseHashState *noise_blake2s_new(void) { return 0; }
NoiseHashState *noise_blake2b_new(void) { return 0; }
NoiseHashState *noise_sha512_new(void) { return 0; }
NoiseDHState *noise_curve448_new(void) { return 0; }
NoiseDHState *noise_kyber_new(void) { return 0; }
NoiseSignState *noise_ed25519_new(void) { return 0; }
