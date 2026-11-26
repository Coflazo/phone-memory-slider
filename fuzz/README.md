# Fuzz targets

libFuzzer targets for every parser that reads bytes from the phone or from a gallery file. Each one runs the parser on
arbitrary input under AddressSanitizer (and UndefinedBehaviorSanitizer with clang) and checks an invariant on top of
"does not crash":

| Target | Code under test | Extra check |
|---|---|---|
| `fuzz_frames` | `FrameDecoder` under the 512-byte and the 65,535-byte cap, fed in reads of 1 to 256 bytes | every frame is 16 bytes to the cap; the frames re-encoded equal the bytes they came from |
| `fuzz_messages` | `decode` | whatever decodes re-encodes to the same bytes, and `encode` never refuses it |
| `fuzz_qr` | `base32_decode`, `decode_pairing` | whatever decodes re-encodes to the same text; the expiry holds through its second, not after |
| `fuzz_handshake` | `ResponderSession::on_message1`, a fresh session and pairing window per input | never `Continue`; message 2 exactly when established; the window burns exactly on a first pairing |
| `fuzz_media_meta` | `parse_motion_photo_xmp`, `find_jpeg_xmp`, `find_jpeg_exif`, `parse_exif` | the JPEG finders return views inside the input |
| `fuzz_sniff` | `sniff_media` | bytes past `sniff_bytes` do not change the answer |

`corpus/<target>/` holds the seeds: the golden messages and handshake transcript from `core/tests/fixtures/`, the QR
test vector from `docs/PROTOCOL.md`, and small JPEG, TIFF, XMP and container heads built like the ones in the core
tests. `python fuzz/make_seeds.py` writes them again.

## Build and run

Clang (what CI runs, job `fuzz` in `.github/workflows/ci.yml`, 120 s per target on every push):

```sh
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPMS_BUILD_FUZZ=ON -DPMS_BUILD_TESTS=OFF -DPMS_BUILD_DESKTOP=OFF
cmake --build build-fuzz --parallel
mkdir -p corpus-frames && ./build-fuzz/fuzz/fuzz_frames -max_total_time=120 corpus-frames fuzz/corpus/frames
```

MSVC (`/fsanitize=fuzzer` with AddressSanitizer; no UBSan on MSVC):

```sh
cmake -S . -B build-fuzz -DPMS_BUILD_FUZZ=ON -DPMS_BUILD_TESTS=OFF -DPMS_BUILD_DESKTOP=OFF
cmake --build build-fuzz --config Release --parallel 2
# clang_rt.asan_dynamic-x86_64.dll is in VC\Tools\MSVC\<version>\bin\Hostx64\x64; put that folder on PATH.
build-fuzz/fuzz/Release/fuzz_frames.exe -max_total_time=300 build-fuzz/corpus/frames fuzz/corpus/frames
```

`PMS_BUILD_FUZZ` instruments every target and skips `tools/`, so use a build folder of its own.

## Local runs

RESULTS_PLACEHOLDER
