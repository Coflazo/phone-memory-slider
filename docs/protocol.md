# Local Phone Protocol v1

The Android companion exposes an authenticated local HTTPS service while its foreground connection session is active. Discovery uses mDNS with manual-IP fallback. Pairing pins the peer certificate after both screens show the same six-digit fingerprint code.

`protocol/pms.proto` is the wire contract. Catalog cursors are opaque to callers, revision-bound, and resumable. Requests cap catalog pages at 1,000 assets and media chunks at an implementation-defined bounded size. Unknown fields are ignored; incompatible major versions fail pairing.

Trash uses a two-phase contract. `TrashBatchPrepare` validates asset existence, current permissions, favorites, and totals without mutating the library. `TrashBatchCommit` is accepted only for the live prepared token and then opens Android's recoverable system-trash confirmation. Replayed completed tokens return the original result. Disconnect invalidates uncommitted tokens.

Media asset IDs are opaque and device-scoped. Paths, MediaStore URIs, filenames, and user-visible titles are not protocol identifiers. Logs redact asset IDs unless the user explicitly exports a diagnostic bundle.

