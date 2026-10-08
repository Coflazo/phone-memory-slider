# Design system

<!-- impeccable:design-schema 1 -->

## Direction

A quiet darkroom. The app stays out of the way everywhere except the review deck, where the photo is the
only thing that matters. Warm neutrals, one typeface family, plain words, and motion that only appears
when something physically moves: a card being thrown, a step being crossed out, thumbnails arriving.

What we avoid on purpose: gradients, glass, glows, oversized rounding, eyebrow labels, metric-card
dashboards, fake charts, decorative copy, percentages we have not calibrated, and colour as the only
signal. When a choice feels like the default an AI would make, pick the plainer one.

## Foundations

- Type: Geist Sans for interface text, Geist Mono for sizes, counts, codes and time estimates. Body 14 px,
  secondary 13 px, labels 12 px minimum. Titles 20/24/32 px, weight 600. Line height 1.45 for body.
- Spacing scale: 4, 8, 12, 16, 24, 32, 48. Screen padding 32 px; panel padding 24 px.
- Radius: 6 controls, 10 panels, 16 the media card. Tag chips use 6, not pills.
- Borders: 1 px, `border` token. Elevation only on the media card (`0 2px 8px rgba(0,0,0,0.25)` dark,
  `0 2px 8px rgba(0,0,0,0.08)` light).
- Icons: 20 px line icons (Lucide set, ISC licence), 16 px inside badges, monochrome, always next to a
  text label or with an accessible name.
- Window: default 1280 x 800, minimum 1024 x 700. Layout uses a 12-column grid with 24 px gutters.

## Colour tokens

Dark is the default; light follows the OS setting. All text and status colours meet WCAG AA (4.5:1) on
`canvas`, `surface` and `raised` in their theme.

| Token | Dark | Light | Use |
|---|---:|---:|---|
| canvas | `#10100E` | `#F2F0EA` | window background, behind the deck |
| surface | `#191915` | `#FBFAF6` | panels, top bar |
| raised | `#25241E` | `#E7E3D9` | hover rows, input fields, QR frame |
| border | `#3B3930` | `#CFC8B8` | 1 px separators |
| textPrimary | `#F2F0E8` | `#171712` | body and titles |
| textSecondary | `#AAA79B` | `#666257` | help text, metadata |
| brand | `#FF5A36` | `#E2461F` | wordmark and focus ring only, never a status |
| keep | `#B9D27B` | `#4A661B` | keep action, kept state |
| delete | `#FF7658` | `#BD3216` | delete action, queued-for-removal state |
| warning | `#F2C15B` | `#7F5800` | unreadable file, low battery, stalled transfer |
| info | `#9DBCF6` | `#315EAF` | "Help me learn" cards, links |

Status is never colour alone: keep shows a check icon and the word "Keep", delete shows a trash icon and
"Delete", unreadable shows a warning icon and the reason.

## Motion

Two kinds of motion, no others.

- Physical motion (the deck, explicitly requested by the product owner, overriding no-slop-ui's
  "no springs" rule for this surface only): the card follows the pointer 1:1, rotates at most 8 degrees,
  and on release either springs back (critically damped, about 260 ms) or is thrown with the release
  velocity (exit 180-240 ms, faster than enter). Swiping right keeps, swiping left deletes (the convention
  people know from swipe apps). The next card rises from 95% scale and 16 px down to the top position in
  220 ms; at rest it sits there, scaled from its bottom edge so a 16 px strip shows below the top card
  and the stack reads as a deck. A video starts 150 ms after the throw settles; until then its poster frame shows.
- Interface motion: 120-180 ms opacity or colour changes, ease-out. Crossing out a finished onboarding
  step draws a 1 px line left to right over 220 ms, then the row text fades to `textSecondary`.
  Thumbnails arriving fade in over 160 ms. Skeleton placeholders pulse opacity 0.5-0.8 over 1.2 s.
- Reduced motion (read from the OS): the deck uses a 120 ms cross-fade instead of throws, the strike line
  appears without drawing, skeletons stop pulsing. Nothing loses information.

## Components

- Top bar (48 px, `surface`, bottom border): wordmark left; the four steps "Connect, Learn, Review,
  Done" as plain text with the current one in `textPrimary` weight 600; right side the phone and link
  state ("Pixel 9 · Bluetooth" with a link icon) and an "Offline" label with a lock icon.
- Buttons: 36 px high, radius 6. Primary = `textPrimary` fill with `canvas` text. Destructive = `delete`
  fill only on the final "Move to Trash" / "Remove" confirmation. Secondary = 1 px border. Focus = 2 px
  `brand` outline offset 2 px. Hit areas never smaller than 32 x 32.
- Step rail (onboarding): numbered rows (number in Geist Mono), title, one line of help, and a status at
  the right (spinner while the app is checking, check icon when detected). Done rows get the strike line.
- Progress line: a stage name, a Geist Mono counter ("3,210 of 12,431"), a 2 px bar, and the ETA text
  from the core ("Ready in about 4 minutes", "Estimating...", "12,431 found").
- Media card: 16 px radius, the card takes the photo's aspect ratio inside a maximum box (640 x 600 at
  1280 x 800) so there are no bars inside the card; never cropped silently. Kind badge
  for video / Live Photo / GIF / screenshot at the top left, duration for videos. While dragging, the side
  in the throw direction shows a label ("Keep" with check, "Delete" with trash) at 0-100% opacity in step
  with the drag distance.
- Tag chip: 24 px high, radius 6, `raised` fill, `textSecondary` text; at most three per card.
- "Why this card" panel (320 px, right of the deck): the evidence in words ("Blurry", "4 near copies,
  this is not the sharpest", "Screenshot from 2 years ago"), the three nearest favorites as 64 px
  thumbnails, and for learning cards the line "Helps the model learn what you keep".
- Duplicate row: 2-4 images side by side at equal height, the suggested best shot marked "Best shot"
  with its reason; clicking another image makes it the keeper.

## Screens

1. Onboarding: "Which phone do you have?" with four choices (Pixel, Samsung Galaxy, Other Android,
   iPhone) and a model field pre-filled from the cable when one is connected; then the step rail for that
   family, each step checked by the app.
2. Connect: USB devices list, or the pairing QR (dark modules on white, 280 px, inside a `raised` frame)
   with a 2-minute countdown; after the scan, the 6-digit code to compare: "Does your phone show 482 913?"
3. Learning: the favorites grid filling in, the stage progress lines with ETA, and a small chart of the
   training loss and held-out recall with the sentence "Learned from 214 favorites".
4. Categories: a table (icon, name, items, size, likely to clear) sorted by space you are likely to free.
5. Review: the deck centred in the space left of the panel, Delete to the left of the card and Keep to
   the right (the buttons mirror the swipe and are the single-pointer alternative to dragging), Undo
   24 px below the card's own bottom edge (it follows the card's height), keyboard hints (Right/K keep,
   Left/D delete, Z undo) listed under the Why panel, the "Why this card" panel, and a header line
   "126 reviewed · 1.8 GB queued".
6. Duplicates: duplicate rows, one per cluster.
7. Done: what will be removed and how much space, the destructive confirmation, then the result and a
   way to restore.
8. Settings: recovery vault (retention, size, purge), paired phones (unpair), model (name, size, GPU or
   CPU), offline (what that means and how it is checked), licenses.

## Writing

Plain, specific, short. Say what happens to the user's photos. Numbers get units and come from
measurement. No exclamation marks, no "magic", no "AI-powered". Recommendation strength is described in
words; a percentage appears only once it is calibrated (see docs/MODEL.md).
