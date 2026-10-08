# Phone link protocol, version 1

This document specifies how the Phone Memory Slider desktop app and its Android companion talk to each other. It is written so that a third party can build a compatible peer. The reference implementation is `core/include/pms/link/` (C++); `core/tests/fixtures/` holds byte-exact test vectors for the messages and the handshake (see [Test vectors](#11-test-vectors)).

The link never uses IP networking. The only transport is Bluetooth Classic RFCOMM.

Roles:

- **Phone**: the Android companion. Noise initiator. Serves the catalog, previews, keyframes, hashes and trash requests.
- **Desktop**: the desktop app. Noise responder. Shows the pairing QR code and sends every request.

## 1. Conventions

| Notation | Wire form |
|---|---|
| `u8`, `u16`, `u32`, `u64` | Unsigned integer, big-endian. |
| `i64` | Signed integer, two's complement, big-endian. |
| `bool` | `u8`; 0 = false, 1 = true. Any other value is a violation. |
| `string` | `u16` byte length, then that many bytes of UTF-8. At most 1024 bytes. The bytes must be well-formed UTF-8: no overlong forms, no surrogates (U+D800 to U+DFFF), nothing above U+10FFFF. |
| `jpeg` | `u16` byte length, then the JPEG bytes. At most 61,440 bytes (60 KB). |
| `sha256` | 32 raw bytes, no length prefix. |
| `list<T>` | `u16` element count, then the elements back to back. Each list has its own cap below. |

A *violation* is any byte sequence that breaks this document: a cap exceeded, a value out of range, a length that runs past the end of the data, or bytes left over after the last field. The receiver of a violation closes the connection. It may send an `Error` with code `0x0001` first.

## 2. Transport

- Bluetooth Classic RFCOMM in insecure mode: no OS pairing dialog, no bonding, no device scan. The phone connects straight to the MAC address and service UUID it read from the QR code.
- The desktop listens only while its Connect or Sync screen is open, serves one session at a time, and refuses further connections while a session is live.
- Every connection runs a fresh handshake (section 4). Nothing from an earlier connection's transport state is reused.

Connection limits enforced by the desktop:

| Limit | Value |
|---|---|
| Handshake must complete within | 5 s of the connection opening |
| Handshake attempts | at most 1 per second |
| Idle timeout | Time since the last received frame: 5 min normally, 10 min while a `TrashRequest` is outstanding (Android's confirmation dialog is user-paced) |

## 3. Pairing QR code

The QR code holds the text `PMS1:` followed by the RFC 4648 Base32 encoding (alphabet `A`-`Z`, `2`-`7`, uppercase, no padding) of a 75-byte payload. The payload encodes to exactly 120 characters, so the whole text is 125 characters and fits QR alphanumeric mode.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Payload version, `1` |
| 1 | 6 | Desktop Bluetooth MAC address, in printed order (`AA:BB:CC:DD:EE:FF` is `AA` first) |
| 7 | 16 | RFCOMM service UUID, in printed order (RFC 4122 byte order) |
| 23 | 32 | Desktop static X25519 public key |
| 55 | 16 | One-time pairing token |
| 71 | 4 | Expiry, `u32` Unix seconds |

The token is valid for 120 seconds and for one successful pairing. The phone accepts only the exact text: `PMS1:` in capitals, then exactly 120 characters of the alphabet, nothing before or after. It refuses a code with another prefix, another length, a payload version other than 1, or any character outside the alphabet (lowercase letters, `0`, `1`, `8`, `9`, `=` padding, whitespace). A code is valid through its expiry second, checked by the desktop against its own clock; the phone does not reject a scanned code by expiry, so a phone clock that runs fast cannot refuse a valid code. 75 bytes are exactly 600 bits, so the 120 characters carry no spare bits. There is no checksum beyond the QR code's own error correction: a misread MAC, UUID, key or token fails the connection or the handshake.

Test vector: the payload whose every byte equals its offset (byte values in hex: version `01` at offset 0, MAC `01:02:03:04:05:06`, UUID bytes `07` to `16`, key bytes `17` to `36`, token bytes `37` to `46`, expiry `0x4748494A`) is the text

```
PMS1:AEAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFYYDENBWHA5DYPSAIJCEMSCKJRHFAUSUKZMFUXC6MBRGIZTINJWG44DSOR3HQ6T4P2AIFBEGRCFIZDUQSKK
```

The QR code contains the desktop's real Bluetooth MAC address. Blur it in screenshots and recordings.

## 4. Handshake

- Pattern: `Noise_IK_25519_ChaChaPoly_SHA256`. The phone is the initiator; it already knows the desktop's static key from the QR code.
- Prologue: the 5 ASCII bytes `PMS/1` followed by the 16 service UUID bytes (21 bytes).
- Each handshake message travels in one frame (section 5). Before the handshake completes the desktop reads exactly one frame and refuses any frame header that announces more than 512 bytes, without allocating for it.

### Message 1, phone to desktop

The Noise payload of message 1 (encrypted to the desktop's static key) is:

| Field | Type | Notes |
|---|---|---|
| kind | `u8` | `1` = first pairing, `2` = reconnect |
| token | 16 bytes | Present only when kind = 1: the token from the QR code |
| model | `string` | Phone model, for example `Pixel 9` |

The payload ends exactly after the model. An unknown kind, a short token, a length that runs past the end, bytes after the model or a model that is not well-formed UTF-8 is refused.

Message 1 must fit the one 512-byte frame the desktop reads before the handshake completes. Its Noise overhead is 96 bytes (32-byte ephemeral key, 48-byte encrypted static key, 16-byte tag), so the payload is at most 416 bytes: the model is at most 397 bytes for kind 1 and 413 bytes for kind 2. A phone with a longer model name shortens it, at a UTF-8 character boundary, before sending; the desktop refuses a message 1 over 512 bytes.

The desktop refuses a phone static key of small order, whatever the kind: the all-zero key, or any key whose X25519 result with a clamped scalar is all zeros. Every Diffie-Hellman result with such a key is zero, so a sender needs no private key to claim it. The check comes before any pin lookup or pin.

Each QR code the desktop shows has one pairing window: its token and expiry. The window is shared by every connection the desktop accepts while that code is up, one handshake per connection, and has three rules:

- Kind 1 is accepted only if the window is open, the expiry has not passed and the token matches (compared in constant time). The token is valid through its expiry second, checked by the desktop against its own clock; the phone does not reject a scanned code by expiry.
- The first kind 1 handshake that completes burns the window. The desktop then pins the phone's static public key together with the model and the pairing time (pairing the same phone again replaces its pin). From then on a second phone presenting the same QR code, on any connection, is refused.
- Nothing else burns the window: not a wrong token, not an expired or malformed attempt, not a refused small-order key, not a reconnect. The desktop also closes the window when it hides the code. If two phones race with the same code, only the first to complete is pinned, and if storing the pin fails the window stays open.

Kind 2 is accepted only if the phone's static key is already pinned. It does not need or touch a pairing window. The desktop keeps the pinned record as it is, including the model from the first pairing. In every other case the desktop closes the connection without replying.

Message 1 never carries a command, so replaying it cannot open a usable session.

### Message 2, desktop to phone

The Noise payload of message 2 is empty; the phone treats a message 2 with a payload as a failed handshake. After it, both sides split the handshake into one transport cipher per direction.

After sending message 2 the desktop sends nothing until it has received the phone's `Hello`. The phone reads message 2 under the same one-frame rule (section 5), so a frame sent right behind message 2 could arrive before the phone has processed it and fail the phone's decoder. The phone's `Hello` is the first transport message in either direction.

### Short authentication string

After a first pairing both screens show a 6-digit code so the user can confirm that no one is in the middle: read the first 4 bytes of the Noise handshake hash as a big-endian `u32`, take it modulo 1,000,000, and print it zero-padded to 6 digits.

### Transport messages

Every frame after message 2 carries exactly one Noise transport message: the ChaChaPoly ciphertext of one plaintext message (section 6) plus a 16-byte authentication tag. Nonces are implicit counters, starting at 0 in each direction. A frame that fails to decrypt closes the connection.

## 5. Frames

| Field | Type | Notes |
|---|---|---|
| length | `u16` | Number of bytes that follow, 16 to 65,535 |
| bytes | `length` bytes | One handshake message, or one transport message |

A receiver checks each length as soon as both header bytes arrive and fails before allocating if it is below 16 or above the current cap: 512 bytes before the handshake completes, 65,535 after. Every frame carries a Noise message, and the shortest one (an empty transport message) is its 16-byte tag, so a shorter frame is a violation. Before the handshake completes exactly one frame is allowed: a second frame that starts before the first has been processed is a violation. Because a transport message adds a 16-byte tag, a plaintext message is at most 65,519 bytes.

## 6. Messages

Every plaintext message has this layout:

| Field | Type | Notes |
|---|---|---|
| type | `u8` | One of the types below. Any other value is a violation. |
| request_id | `u32` | See below |
| payload | depends on type | Must end exactly at the end of the plaintext |

`request_id`: the desktop picks a non-zero id for each request, unique among its outstanding requests, and every response to that request carries the same id. `Hello`, `HelloAck` and `Bye` use 0. An `Error` carries the id of the request it answers, or 0 when it concerns the whole session.

| Type | Name | Direction | Answered by |
|---|---|---|---|
| `0x01` | Hello | phone to desktop | HelloAck |
| `0x02` | HelloAck | desktop to phone | |
| `0x10` | CatalogSince | desktop to phone | one CatalogPage |
| `0x11` | CatalogPage | phone to desktop | |
| `0x12` | PreviewRequest | desktop to phone | one Preview per id |
| `0x13` | Preview | phone to desktop | |
| `0x14` | KeyframesRequest | desktop to phone | one Keyframes |
| `0x15` | Keyframes | phone to desktop | |
| `0x16` | HashRequest | desktop to phone | one Hash per id |
| `0x17` | Hash | phone to desktop | |
| `0x20` | TrashRequest | desktop to phone | one TrashResult |
| `0x21` | TrashResult | phone to desktop | |
| `0x30` | Error | either | |
| `0x31` | Bye | either | |

A message that is well formed but arrives in the wrong direction, out of order, or with a `request_id` that matches no outstanding request is answered with `Error` `0x0004` and the connection is closed.

Session order:

1. The phone sends `Hello`. Until it arrives the desktop sends nothing (section 4).
2. The desktop answers `HelloAck`, or sends `Error` `0x0002` and closes if it cannot speak the phone's protocol version.
3. The desktop sends requests; the phone answers them. In protocol 1 the phone sends no requests.
4. Either side may send `Bye` and close.

Every message must fit in 65,519 bytes. The list caps below are upper bounds; the message size limit often binds first, and senders must stop filling a list before it does.

### Hello `0x01`

| Field | Type | Notes |
|---|---|---|
| protocol | `u16` | `1` |
| model | `string` | Phone model |
| os | `string` | For example `Android 15` |
| app_version | `string` | Companion app version |
| capabilities | `u32` | Bit field. Protocol 1 defines no bits: send 0, ignore bits you do not know. |

### HelloAck `0x02`

| Field | Type | Notes |
|---|---|---|
| protocol | `u16` | The version the desktop will speak, `1` |
| desktop_name | `string` | Shown on the phone |

### CatalogSince `0x10`

| Field | Type | Notes |
|---|---|---|
| store_version | `string` | `store_version` from the last CatalogPage received; empty on a first sync |
| generation | `u64` | `next_generation` from the last CatalogPage received; 0 on a first sync |
| max_items | `u32` | At most 2,000 |

### CatalogPage `0x11`

| Field | Type | Notes |
|---|---|---|
| store_version | `string` | The phone's current media store version (`MediaStore.getVersion`) |
| next_generation | `u64` | Cursor to send in the next CatalogSince |
| full_resync | `bool` | See section 8 |
| more | `bool` | True if the desktop should request another page |
| items | `list<CatalogItem>` | At most 2,000, and at most `max_items` |
| removed | `list<u64>` | At most 20,000 ids that no longer exist on the phone |

CatalogItem, 58 fixed bytes (including both `u16` string lengths) plus the string bytes:

| Field | Type | Notes |
|---|---|---|
| id | `u64` | MediaStore `_ID`, stable for the life of the item |
| media_type | `u8` | `1` = image, `3` = video (MediaStore values). Any other value is a violation. |
| relative_path | `string` | Folder and file name, for example `DCIM/Camera/IMG_0001.jpg` |
| mime | `string` | For example `image/jpeg` |
| size | `u64` | Bytes |
| width | `u32` | Pixels |
| height | `u32` | Pixels |
| duration_ms | `u32` | 0 for images |
| date_taken_ms | `i64` | Milliseconds since the Unix epoch; -1 if unknown |
| date_modified_s | `i64` | Seconds since the Unix epoch (MediaStore `DATE_MODIFIED`) |
| generation_modified | `u64` | MediaStore `GENERATION_MODIFIED` |
| favorite | `bool` | MediaStore `IS_FAVORITE` |

### PreviewRequest `0x12` and HashRequest `0x16`

| Field | Type | Notes |
|---|---|---|
| ids | `list<u64>` | 1 to 16 ids; an empty list is a violation |

The phone answers each id with its own `Preview` (or `Hash`) message, in any order, all carrying the request's `request_id`.

### Preview `0x13`

| Field | Type | Notes |
|---|---|---|
| id | `u64` | Item id |
| status | `u8` | Item status, below |
| width | `u32` | Pixels of the JPEG |
| height | `u32` | Pixels of the JPEG |
| jpeg | `jpeg` | At most 61,440 bytes. A JPEG whose long side is 256 px. |

Item status:

| Value | Name | Meaning |
|---|---|---|
| 0 | Ok | The data fields are valid |
| 1 | Missing | The item no longer exists |
| 2 | Unreadable | The item exists but could not be read or decoded |
| 3 | Busy | The phone cannot serve it now; ask again later |

Any other status value is a violation. When status is not Ok, the sender puts zeros or empty values in the data fields (`width`, `height`, `jpeg`, `jpegs`, `sha256`) and the receiver ignores them. A missing or unreadable item never ends the session.

### KeyframesRequest `0x14`

| Field | Type | Notes |
|---|---|---|
| id | `u64` | Video id |
| count | `u8` | 1 to 8 frames; 0 or more than 8 is a violation |

### Keyframes `0x15`

| Field | Type | Notes |
|---|---|---|
| id | `u64` | Video id |
| status | `u8` | Item status |
| jpegs | `list<jpeg>` | At most 8 frames of at most 61,440 bytes each, spread over the video's duration |

The phone answers a `KeyframesRequest` with exactly one `Keyframes` message carrying at most `count` JPEGs (fewer if the video is too short or a frame cannot be decoded). All frames share that one message, so together they must fit in 65,519 bytes; in practice each is well under 8 KB.

### Hash `0x17`

| Field | Type | Notes |
|---|---|---|
| id | `u64` | Item id |
| status | `u8` | Item status |
| sha256 | `sha256` | SHA-256 of the item's current bytes |

### TrashRequest `0x20`

| Field | Type | Notes |
|---|---|---|
| items | `list<TrashItem>` | 1 to 500 items; an empty list is a violation |

TrashItem, 56 bytes:

| Field | Type | Notes |
|---|---|---|
| id | `u64` | Item id |
| size | `u64` | Size the desktop reviewed |
| date_modified_s | `i64` | `date_modified_s` the desktop reviewed |
| sha256 | `sha256` | Hash the desktop reviewed |

### TrashResult `0x21`

| Field | Type | Notes |
|---|---|---|
| items | `list<TrashEntry>` | One entry per requested item, in request order: 1 to 500 entries; an empty list is a violation |

TrashEntry, 9 bytes: `id` (`u64`) then `outcome` (`u8`):

| Value | Name | Meaning |
|---|---|---|
| 0 | Trashed | Moved to Android's system Trash |
| 1 | UserDeclined | The user declined Android's confirmation dialog |
| 2 | Changed | Size, modification date or hash no longer match the request |
| 3 | Missing | The item no longer exists |
| 4 | Favorite | The item is a favorite; favorites are never trashed |
| 5 | Failed | Any other failure |

Any other outcome value is a violation.

### Error `0x30`

| Field | Type | Notes |
|---|---|---|
| code | `u16` | Section 9 |
| message | `string` | Diagnostic text for logs |

### Bye `0x31`

Empty payload. The sender closes the connection after sending it; the receiver closes on receipt.

## 7. Flow control

Flow control is pull-based: a request is a credit.

- The desktop never has more than 16 responses outstanding. A `PreviewRequest` or `HashRequest` with n ids uses n credits; every other request uses one.
- Each response message (`CatalogPage`, `Preview`, `Keyframes`, `Hash`, `TrashResult`) returns one credit. An `Error` carrying a request's id ends that request and returns all of its remaining credits.
- `Hello`, `HelloAck`, `Error` and `Bye` use no credits.
- A phone that receives a request beyond 16 outstanding credits sends `Error` `0x0003` and closes.

## 8. Catalog sync and resume

The desktop treats `store_version` and `next_generation` as an opaque cursor: it stores the pair from each page and sends it back unchanged in the next `CatalogSince`. The cursor only advances when the desktop echoes it, so a page lost to a dropped connection is simply sent again.

Desktop rules:

- Insert or replace each item by `id`. The same id may arrive more than once; the latest copy wins.
- Delete every id in `removed`. Deleting an id it does not hold is a no-op.
- When `full_resync` is true, first mark every item held for this phone as unconfirmed. Mark every item received during the resync as confirmed. When a page with `more` = false arrives, delete the items still unconfirmed. Both marks must persist, so a resync interrupted by a dropped connection finishes correctly on the next one.
- Keep requesting while `more` is true.

Reference phone behaviour:

- If the request's `store_version` differs from the current one (always so on a first sync), the phone starts from the beginning and sets `full_resync` on the first page of that pass.
- Otherwise it sends items with `generation_modified` greater than `generation`, in ascending `generation_modified` then `id` order, filling a page until `max_items` or the 65,519-byte limit. `next_generation` is the highest generation whose items have all been sent. If one generation does not fit in a page, the phone sends `next_generation` unchanged with `more` = true and remembers, for that connection only, how far into the generation it got; a new connection restarts that generation, which the desktop absorbs by replacing by id.
- `removed` lists ids the phone has reported before that no longer exist. The phone keeps reporting such an id until the desktop echoes a cursor from a page at or after the one that carried it.

Previews, keyframes and hashes are stateless: after a reconnect the desktop requests again any id it has no answer for.

A `TrashRequest` outstanding when the connection drops has an unknown outcome. The desktop must not assume either result. On reconnect it syncs the catalog first (trashed items arrive in `removed`) and sends again only items that still exist; the phone validates them again, so a repeat is safe.

### Trash binding

Each `TrashItem` binds the request to the exact item the user reviewed. For each item the phone:

1. Returns Missing if the id no longer exists.
2. Returns Favorite if the item is a favorite.
3. Returns Changed unless the current size, `DATE_MODIFIED` and SHA-256 of the current bytes all equal the request.
4. Passes the remaining items to `MediaStore.createTrashRequest`, so Android's own confirmation dialog is the final gate, and returns Trashed or UserDeclined from the user's choice, or Failed.

The desktop splits a larger selection into requests of at most 500 items and sends the next one after the previous `TrashResult` arrives. Trashed items stay in Android's Trash for about 30 days.

## 9. Error codes

| Code | Name | Meaning | Connection |
|---|---|---|---|
| `0x0001` | Malformed | A frame or message is a violation of this document | Closed |
| `0x0002` | UnsupportedProtocol | The peer's protocol version is not supported | Closed |
| `0x0003` | FlowControl | More than 16 credits outstanding | Closed |
| `0x0004` | UnexpectedMessage | Wrong direction, wrong order, or unknown `request_id` | Closed |
| `0x0005` | PermissionDenied | The phone has lost read access to media | Open |
| `0x0006` | Busy | The phone cannot serve this request now, for example while a Trash dialog is open; ask again later | Open |
| `0x0007` | Internal | Unexpected failure while serving the request | Open |

The sender of a code marked Closed closes the connection right after sending it. Receivers treat unknown codes as Internal.

## 10. Implementation notes

- Frame decoder memory: completed frames wait until the caller takes them, so a decoder holds at most one read's bytes plus the cap plus 2. Feed reads of at most 64 KiB and take every completed frame after each read.
- Lists: grow a list one parsed element at a time instead of reserving from the count field, so memory follows the bytes actually received.
- Encoders: refuse to send anything the decoder would refuse. The reference `encode()` runs its own decoder on every message before returning it.

## 11. Test vectors

`core/tests/fixtures/golden_messages.bin` holds one plaintext message of every type, in ascending type order, each preceded by its `u16` big-endian length. It is not a frame stream: some plaintext messages, such as `Bye`, are shorter than the 16-byte frame minimum. The field values are in `golden_set()` in `core/tests/link_messages_tests.cpp`. A compatible encoder reproduces the file byte for byte, and a compatible decoder accepts every message in it. The C++ test suite checks the file on every run; regenerate it with `pms_core_tests --write-golden core/tests/fixtures` only for a deliberate protocol change.

`core/tests/fixtures/golden_ik_transcript.bin` is one first pairing (kind 1) followed by four transport messages, generated from fixed keys by the reference sessions. It is 22 fields back to back, each a `u16` big-endian length followed by that many bytes:

| # | Field | Bytes |
|---|---|---|
| 1 | Desktop static private key | 32 |
| 2 | Desktop static public key | 32 |
| 3 | Desktop ephemeral private key | 32 |
| 4 | Phone static private key | 32 |
| 5 | Phone static public key | 32 |
| 6 | Phone ephemeral private key | 32 |
| 7 | Service UUID | 16 |
| 8 | Prologue (`PMS/1` + UUID) | 21 |
| 9 | Token | 16 |
| 10 | Model, UTF-8 (`Pixel 9`) | 7 |
| 11 | Message 1 payload (kind 1, token, model) | 26 |
| 12 | Message 1 | 122 |
| 13 | Message 2 | 48 |
| 14 | Short authentication string, 6 ASCII digits | 6 |
| 15, 16 | Transport 1, phone to desktop: plaintext `phone 1`, ciphertext | 7, 23 |
| 17, 18 | Transport 2, desktop to phone: plaintext `desktop 1`, ciphertext | 9, 25 |
| 19, 20 | Transport 3, phone to desktop: plaintext `phone 2`, ciphertext | 7, 23 |
| 21, 22 | Transport 4, desktop to phone: empty plaintext, ciphertext (a bare tag) | 0, 16 |

The private keys are the bytes `00`..`1F` (desktop static), `20`..`3F` (desktop ephemeral), `40`..`5F` (phone static) and `60`..`7F` (phone ephemeral), each clamped as X25519 clamps (first byte `& F8`, last byte `& 7F | 40`). The token is `A0`..`AF` and the UUID is `8f4d9d6a-0c84-4a7f-a36a-6fc12c4b34bf`. A compatible phone, given fields 4, 6, 2, 7, 9 and 10 (its keys with the ephemeral key fixed, the desktop key, UUID, token and model), produces message 1 byte for byte, accepts message 2, shows the same 6 digits, and seals and opens the transport messages identically; a compatible desktop does the same from its side. The C++ suite checks the file on every run and regenerates it with `pms_core_tests --write-golden core/tests/fixtures`. An independent implementation of the Noise specification reproduces it.

`core/tests/fixtures/cacophony_ik.txt` holds the Cacophony vector for `Noise_IK_25519_ChaChaPoly_SHA256` (source and filter in its header). The reference handshake reproduces its messages, handshake hash and transport messages byte for byte.
