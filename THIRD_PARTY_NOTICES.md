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
