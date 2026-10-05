# Privacy and data handling

Phone Memory Slider is designed to work indefinitely without an internet connection. The product executable has no account flow, telemetry, advertising SDK, updater, cloud inference, remote font, HTTP client, or developer-operated service.

## Data read from the phone

The app sees only media exposed by the operating system through Windows Portable Devices or a local/mounted folder selected by the user. An unlocked phone and explicit trust/File Transfer approval may be required. Cloud-only media, secure folders, and any content hidden by the phone remain unavailable.

For each accessible item, the app reads basic metadata, a preview for review/analysis, and the complete byte stream for SHA-256 verification. Native favorite metadata is not consistently exposed by USB protocols. Visible `Favorites`/`Favourites` folders are detected automatically; when album membership is hidden, the user selects or exports local keep examples instead.

## Data stored locally

- SQLite catalog metadata, compact visual features, reason tags, and review decisions.
- User-selected keep examples or their locally computed feature vectors.
- Temporary previews in the operating-system cache.
- Encrypted recovery payloads and a local JSONL manifest.
- A local vault key: DPAPI-protected on Windows; owner-readable-only key file on macOS/Linux.

The preference model does not identify or cluster people. A binary face-present feature may be used as one visual dimension, but no identity, biometric template, or person name is produced.

## Deletion transaction

Nothing is removed during ranking or swiping. After the user confirms a queued batch, each item is copied to a temporary local file and compared with the SHA-256 recorded during analysis. It is then encrypted with AES-256-GCM, authenticated through an in-memory verification sink, checked for size and SHA-256 equality, rechecked at the source, and only then removed. If the item changed or any copy, authentication, or checksum step fails, the source is retained.

The recovery vault is not a substitute for a backup. Keep a separate backup before using any gallery-management tool.

## Network boundary

The desktop runtime does not link Qt Network and contains no network client. [`scripts/check-repository.ps1`](../scripts/check-repository.ps1) fails if common networking primitives, URL literals, or the retired phone-transport directories return to runtime source.

The opt-in evaluation downloader under `scripts/` does use the internet to fetch openly licensed test images. It is a developer tool, is never called by the application, and is not packaged into the runtime.

## Removing local data

Delete the platform application-data folder named `Phone Memory Slider` to erase the catalog, model state, preview cache, vault key, manifest, and encrypted recovery files. Removing this local data does not modify the phone gallery. Deleting the vault key makes existing encrypted recovery payloads unreadable.
