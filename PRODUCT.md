# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Stack

Qt Quick/QML and C++23 for Windows, macOS, and Linux; Kotlin and Jetpack Compose for the Android companion. Desktop is the analysis host.

## Users

People with large Android photo libraries who want to recover storage without uploading private media or surrendering deletion decisions to automation.

## Product Purpose

Phone Memory Slider indexes user-authorized, locally stored phone media, learns what the user values from favorites and explicit review decisions, and ranks likely cleanup candidates in a fast swipe workflow. Success means useful recommendations, explainable reasons, recoverable deletion, smooth photo/video review, and no user media leaving the local network.

## Positioning

Personal preference learning and cleanup analysis happen entirely between the user's phone and desktop. The product combines deterministic cleanup evidence with a lightweight personal model and always leaves the final trash decision to the user.

## Operating Context

The Android companion is open in a foreground connection session. The desktop pairs on local Wi-Fi or a USB-tethered local network, performs a resumable scan, and presents a ranked review deck. Trash decisions remain queued until the user confirms the session on both desktop and Android.

## Capabilities and Constraints

- Android 11+ first; iPhone support is a later companion implementation.
- Local, user-authorized media only. Cloud-only, locked, secure-folder, and unavailable assets are excluded and reported.
- Full versus partial gallery access is always disclosed.
- Favorites and manually kept assets are protected from trash batches.
- No accounts, cloud APIs, analytics, external inference, or silent deletion.
- Target scale is 250,000 assets.

## Brand Commitments

Working name: Phone Memory Slider. Dark-first, restrained product interface with a cinematic, physics-driven review surface. Heavy motion must remain purposeful and have a reduced-motion equivalent.

## Evidence on Hand

No production logo, customer media, testimonials, or performance claims exist. Development and tests use generated fixtures only.

## Product Principles

1. Private by construction.
2. Explain recommendations in ordinary language.
3. Preserve human control and recoverability.
4. Spend complexity only where measurements justify it.
5. Keep review fast enough to feel physical.

## Accessibility & Inclusion

WCAG AA contrast, complete keyboard operation, screen-reader labels, non-color status cues, and a reduced-motion mode are release requirements.

