# Design System

<!-- impeccable:design-schema 1 -->

## Direction

Operate mode. The application is quiet, precise, and familiar outside the review deck. The deck is the signature moment: full-bleed media, spring physics, restrained depth, and clear evidence for each recommendation.

## Foundations

- Typography: Geist Sans for UI and Geist Mono for storage, counts, and technical values.
- Spacing: 4, 8, 12, 16, 24, 32, 48.
- Radius: 8 for controls, 12 for panels, 16 for the media card. Pills are reserved for compact semantic tags.
- Motion: 90 ms immediate, 180 ms controls, 420 ms expressive, 700 ms major state transition.
- No glassmorphism, decorative gradients, oversized rounding, fake charts, custom cursors, or decorative status dots.

## Color Tokens

| Token | Dark | Light |
|---|---:|---:|
| canvas | `#0B0D10` | `#F6F7F9` |
| surface | `#12161B` | `#FFFFFF` |
| raised | `#181D24` | `#EEF1F5` |
| border | `#2A313B` | `#D8DEE7` |
| text-primary | `#F3F5F7` | `#15181D` |
| text-secondary | `#A9B0BA` | `#5E6672` |
| accent | `#6B7CFF` | `#4F63E8` |
| keep | `#46C987` | `#218A55` |
| delete | `#FF5C67` | `#C73544` |
| warning | `#F4B860` | `#9A6500` |

## Interaction Rules

- Drag follows the pointer with capped rotation and depth; release uses velocity and distance.
- Keyboard actions mirror gestures. Undo is always available for queued decisions.
- Video starts muted and exposes play, mute, and scrub controls without stealing swipe input.
- Reduced motion replaces spatial transitions with opacity changes under 180 ms.
- Recommendation strength is qualitative. Never present an uncalibrated percentage as probability.

