# Product

<!-- impeccable:product-schema 1 -->

## Platform

Adaptive desktop application. Windows is the primary direct-USB target; macOS and Linux use mounted or exported gallery folders until native device connectors are validated.

## Stack

Qt Quick/QML and C++23. Windows phone access uses native Windows Portable Devices APIs. Analysis, preference learning, SQLite state, and encrypted recovery all run locally.

## Users

People with large phone galleries who want to recover storage without uploading private media or delegating deletion decisions to automation.

## Product purpose

Phone Memory Slider indexes media exposed over USB or a local folder, learns what the user values from a deliberately selected set of keep photos, ranks likely cleanup candidates, and turns review into a fast swipe workflow. Success means useful ordering, honest explanations, recoverable deletion, smooth photo/video review, and zero runtime egress.

## Positioning

Personal preference learning happens on the user's computer. Deterministic duplicate and quality evidence is combined with a lightweight personal model, but the person always makes the final decision.

## Operating context

The user connects and unlocks a phone or selects a mounted/exported DCIM folder. The app catalogs exposed media, asks for 20–50 local keep examples, analyzes the gallery, and presents a ranked deck. Selected removals are copied into an authenticated encrypted vault and verified before the original is removed.

## Capabilities and constraints

- Direct Windows Portable Devices support first; folder sources work cross-platform.
- Only OS-exposed, locally readable media is in scope. Cloud-only items, secure folders, and unavailable assets are excluded.
- Native phone favorite flags are not reliably exposed over USB. Visible `Favorites`/`Favourites` folders are learned automatically; the user supplies keep examples when album metadata is hidden.
- Favorites and explicit keeps are protected from cleanup ranking.
- No account, cloud API, analytics, external inference, phone companion, or silent deletion.
- Target catalog scale is 250,000 assets; real-phone performance still requires hardware acceptance.

## Brand commitments

Working name: Phone Memory Slider. Quiet, editorial interface with a cinematic, physics-driven review surface. Motion is purposeful and has a reduced-motion equivalent.

## Product principles

1. Private by construction.
2. Explain recommendations in ordinary language.
3. Preserve human control and recoverability.
4. Spend complexity only where measurements justify it.
5. Keep review fast enough to feel physical.

## Accessibility and inclusion

WCAG AA contrast, complete keyboard operation, screen-reader labels, non-color status cues, and reduced motion are release requirements.
