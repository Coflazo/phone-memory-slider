# Privacy and data handling

Phone Memory Slider is designed to work without an account or internet service.

## Data that stays local

- The Android companion reads only media granted through Android permissions.
- Gallery metadata, small thumbnails, and requested video chunks travel over an authenticated TLS connection on the local network.
- The desktop stores opaque asset IDs, metadata, compact visual features, and the resumable sync cursor in a local SQLite database. Review choices stay in the active session until confirmation.
- Preference-model weights and face-presence signals stay on the desktop. The app does not identify or cluster people.
- A temporary file is used for the current video and removed when the card changes or the app exits.

## Data that is not collected

There is no sign-in, advertising ID, telemetry, analytics, crash upload, cloud inference, remote API, or developer-operated server. MediaStore paths and content URIs are not exposed as protocol IDs. Runtime diagnostics use safe messages and do not intentionally log asset identifiers or pairing tokens.

## Deletion safety

The desktop can only queue candidates. Favorites and explicit keeps are protected by the ranking queue. The phone checks current favorite state again before requesting Android's recoverable system trash. There is no permanent-delete fallback.

## Removing local data

Uninstalling the Android app removes its app-private key material. Uninstalling the desktop app may not remove its local catalog automatically; delete the platform application-data folder named `Phone Memory Slider` if you want to erase learned features and decisions. Removing that database does not change the phone gallery.
