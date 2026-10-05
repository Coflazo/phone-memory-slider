# Security policy and architecture

## Supported versions

Security fixes are applied to the latest commit on the default branch. No production binary has been declared supported yet.

## Reporting a vulnerability

Do not include private media, device identifiers, vault keys, or credentials in a public issue. Use GitHub's private vulnerability-reporting feature for this repository when enabled. If it is unavailable, open a minimal issue asking the maintainer to enable a private channel.

## Trust boundaries

- Phone metadata and content are untrusted input.
- Local folder paths are accepted only from an explicit user selection and are canonically re-contained before every read, copy, hash, or removal.
- Previews are decoded by Qt and platform codecs in bounded analysis paths.
- Device content is streamed rather than loaded whole for hashing, copying, encryption, and verification.
- Source deletion is allowed only after an authenticated recovery copy passes size and SHA-256 verification and the source still matches the exact digest analyzed during review.
- The product runtime has no network feature or remote service dependency.

## Cryptography

Recovery payloads use AES-256-GCM with a fresh 96-bit nonce and 128-bit authentication tag. Every archive receives a unique UUID filename, so repeated device object IDs cannot overwrite earlier recovery copies. Verification decrypts into a hashing sink without materializing plaintext in the vault. The file format includes a magic value, version, original size, nonce, and tag. Windows generates the vault key with the OS CSPRNG and stores it protected by DPAPI for the current user. macOS/Linux generate the key with OpenSSL and restrict the key file to the owning user; native platform key stores are a documented hardening item.

Cryptography protects local recovery copies at rest and detects tampering. It does not protect a compromised logged-in account, a malicious codec, or a machine that is already under attacker control.

## Zero-egress enforcement

The product implements no HTTP, sockets, telemetry, updates, or remote inference, and the executable has no direct Qt Network import. Qt Quick and Qt Multimedia depend transitively on Qt's shared network library; release packaging excludes Qt network-information, TLS, and TCP QML-debug plugins. CI runs source and package guards. Release acceptance additionally requires a deny-all firewall run and packet capture because static checks alone cannot prove behavior of every linked platform component.

## Known release risks

- Windows Portable Devices behavior varies by phone, driver, and permission state.
- Native macOS Keychain and Linux Secret Service key storage are not implemented.
- The real-device matrix, codec fuzzing, binary signing, and independent security review are incomplete.
- The recovery vault is not a backup; losing its key loses access to its encrypted payloads.
