# Local Phone Protocol v1

The Android companion exposes an authenticated local HTTPS service while its foreground connection session is active. Discovery uses mDNS with manual-IP fallback. The phone creates a fresh random six-digit code for each session, locks pairing after five failed attempts, and the desktop pins the certificate that successfully authenticated the code.

`protocol/pms.proto` is the semantic contract. The beta transports its fields as versioned JSON over these endpoints:

- `POST /v1/pair`
- `GET /v1/capabilities`
- `GET /v1/catalog?limit=&cursor=`
- `GET /v1/thumbnail?asset_id=&max_edge=`
- `GET /v1/content?asset_id=` with one HTTP byte range
- `GET /v1/hash?asset_id=`
- `POST /v1/trash/prepare`
- `POST /v1/trash/commit`
- `GET /v1/trash/result?token=`

Every endpoint except pairing requires the rotating bearer token. Catalog cursors are opaque to callers, revision-bound, and resumable. A stale cursor returns HTTP 409 and the desktop restarts without mixing revisions. Requests cap catalog pages at 1,000 assets, thumbnails at 2,048 px/5 MB, and content chunks at 8 MB. Incompatible major versions fail after capabilities are read.

Trash uses a two-phase contract. `TrashBatchPrepare` validates asset existence, current permissions, favorites, and totals without mutating the library. `TrashBatchCommit` is accepted only for the live prepared token and then opens Android's recoverable system-trash confirmation. Replayed completed tokens return the original result. Disconnect invalidates uncommitted tokens.

Media asset IDs are opaque and device-scoped. Paths, MediaStore URIs, filenames, and user-visible titles are not protocol identifiers. Logs redact asset IDs unless the user explicitly exports a diagnostic bundle.
