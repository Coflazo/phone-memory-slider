# Third-party notices

## Geist fonts

Copyright 2023 The Geist Project Authors.

Geist and Geist Mono are distributed under the SIL Open Font License 1.1. The complete license text is included at `third_party/geist/OFL.txt`.

Source: https://github.com/vercel/geist-font/releases/tag/v1.7.2

## Qt

The application uses Qt 6. Licensing depends on how binaries are distributed and linked. See https://www.qt.io/licensing and include the applicable Qt license texts with release packages.

## OpenSSL

Non-Windows recovery-vault builds use OpenSSL 3. See https://www.openssl.org/source/license.html. Windows uses native BCrypt and DPAPI instead.

## noise-c

Copyright (C) 2016 Southern Storm Software, Pty Ltd. MIT License; the full text is at `third_party/noise-c/LICENSE`.

The phone link's Noise handshake uses a subset of noise-c (Signal's fork, commit `6d940358181c0b1d9355521aebd0d5dc6ca0d0f9`), including curve25519-donna and poly1305-donna (MIT or public domain) and ed25519-donna (public domain). Source and file list: `third_party/noise-c/README.md`.

Source: https://github.com/signalapp/noise-c

## PDQ perceptual hash

The duplicate finder uses the PDQ image hash from Meta's ThreatExchange, vendored in `third_party/pdq/` (source commit and patches in `third_party/pdq/README.md`). `hashing/torben.{h,cpp}` is public domain (Torben Mogensen, N. Devillard). The rest is distributed under this BSD licence, also at `third_party/pdq/LICENSE`:

```text
BSD License

For ThreatExchange software

Copyright (c) Meta Platforms, Inc. and affiliates.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific
   prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
