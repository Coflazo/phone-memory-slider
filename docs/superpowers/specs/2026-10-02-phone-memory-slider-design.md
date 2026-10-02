# Phone Memory Slider Design Specification

## Product Boundary

Ship an open-source, local-only Android gallery cleanup assistant with desktop clients for Windows, macOS, and Linux. Android supplies authorized media metadata and bounded proxies; the desktop owns analysis and ranking. V1 never accesses cloud-only media, identifies people, sends telemetry, or permanently deletes media.

## System Design

The desktop uses Qt Quick/QML over a C++23 core. The Android companion uses Kotlin/Compose and exposes MediaStore through an authenticated foreground service. Pairing occurs only on a local Wi-Fi or USB-tethered network. The versioned protocol carries capabilities, catalog pages and deltas, proxy/keyframe requests, ranged streams, queued trash batches, permission changes, and progress events.

The desktop persists source metadata, FP16 embeddings, deterministic features, labels, scores, decisions, and sync cursors. Originals are streamed only for review and are not retained by default. Analysis work runs through bounded background queues so indexing cannot starve the UI.

## Ranking Contract

Favorites and explicit keeps are protected. A fixed visual encoder produces embeddings; deterministic analysis produces duplicate, blur, exposure, format, face-presence, and storage features. Unfavorited media is unlabeled, not negative. The incremental preference model uses favorites as strong positives and explicit swipes as higher-weight labels. Personal weighting remains disabled until at least 20 favorites or a 30-card calibration exists.

The review priority is a confidence-gated blend of cleanup evidence, personal mismatch, and normalized storage benefit. Every result carries one or more human-readable reason codes. Exact duplicate candidates are confirmed cryptographically only after cheap bucketing narrows the set. Near duplicates use approximate nearest neighbors plus temporal constraints.

## Deletion Contract

A delete swipe changes only local review state. At session end the user reviews count, bytes, categories, and samples. The Android companion then requests recoverable system trash in platform-sized batches. Partial approval, vanished assets, permission revocation, and disconnects remain visible and retryable. There is no permanent-delete fallback.

## Experience Contract

The interface is dark-first with a complete system-controlled light theme. The review deck is cinematic and physics-driven; all other surfaces prioritize scanability. Photo and video actions work by pointer, touchpad, keyboard, and accessible controls. Reduced motion, WCAG AA contrast, visible focus, and non-color state cues are mandatory.

## Performance Contract

The architecture must handle a synthetic 250,000-asset catalog, refresh ranking in under two seconds once features exist, stay under 1.5 GB steady memory, and keep persistent analysis data under 3 GB excluding user-configured cache. Warm photo cards target 250 ms median and 1 s p95; streamed video targets 1.5 s p95 to first frame on the reference LAN. UI work never performs inference or decoding on the render thread.

