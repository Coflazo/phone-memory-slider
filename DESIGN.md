# Design System

<!-- impeccable:design-schema 1 -->

## Direction

Local film-lab mode. The application is warm, precise, and quiet outside the review deck. The deck is the signature moment: full-bleed media, spring physics, restrained depth, and clear evidence for each recommendation.

## Foundations

- Typography: Geist Sans for UI and Geist Mono for storage, counts, and technical values.
- Spacing: 4, 8, 12, 16, 24, 32, 48.
- Radius: 8 for controls, 12 for panels, 16 for the media card. Pills are reserved for compact semantic tags.
- Motion: 90 ms immediate, 180 ms controls, 420 ms expressive, 700 ms major state transition.
- No glassmorphism, decorative gradients, oversized rounding, fake charts, custom cursors, or decorative status dots.

## Color Tokens

| Token | Dark | Light |
|---|---:|---:|
| canvas | `#10100E` | `#F2F0EA` |
| surface | `#191915` | `#FBFAF6` |
| raised | `#25241E` | `#E7E3D9` |
| border | `#3B3930` | `#CFC8B8` |
| text-primary | `#F2F0E8` | `#171712` |
| text-secondary | `#AAA79B` | `#666257` |
| accent | `#FF5A36` | `#FF5A36` |
| keep | `#B9D27B` | `#52711F` |
| delete | `#FF7658` | `#BD3216` |
| warning | `#F2C15B` | `#8C6200` |
| cool | `#9DBCF6` | `#315EAF` |

## Interaction Rules

- Drag follows the pointer with capped rotation and depth; release uses velocity and distance.
- Keyboard actions mirror gestures. Undo is always available for queued decisions.
- Video starts muted and exposes play, mute, and scrub controls without stealing swipe input.
- Reduced motion replaces spatial transitions with opacity changes under 180 ms.
- Recommendation strength is qualitative. Never present an uncalibrated percentage as probability.
