# Plan D: security gates, docs, README, demo video, release

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove v2 is safe and offline, explain it honestly, show it in a chaotic YC-style demo built from real footage, and ship v2.0.0 under Coflazo.

**Architecture:** Gates first (Cloudflare security-audit, cso, santa-method double review), then documents written from measured artifacts (`evaluation/report.json`, `docs/spikes.md`, acceptance checklists), then the README, then the demo video from real captures, then the release. Nothing in the README is claimed without a file in the repo that backs it.

**Tech Stack:** security-audit skill (validator in a Linux container or CI), gitleaks, libFuzzer, HyperFrames (Apache-2.0) via the brag skill, ffmpeg 8.1 `gfxcapture`, `adb screenrecord`, gh CLI.

**Spec:** `C:\Users\Cagan\.claude\plans\okey-i-want-you-dapper-thacker.md` (Security, README, Demo video, Verification), `DESIGN.md` (Writing), `.superpowers/competitors-2026-10-07.md`.

## Global Constraints

- Every number in README/docs comes from a committed artifact (report.json, spikes.md, acceptance checklist) and states its date and hardware.
- The offline claim reads exactly: "The app never opens a network connection. The Qt and FFmpeg libraries it ships contain network code that the app does not call; CI runs the full test suite inside a network namespace with no interfaces and checks that a request to the internet fails."
- README: at most 2 badges (CI, license); no stat-card widgets; demo GIF at the top linking `demo/demo.mp4`; writing passes stop-slop -> humanizer -> voice, the last two mandatory.
- License changes to MIT (solo work, CLAUDE.md GitHub rule); THIRD_PARTY_NOTICES.md lists every bundled component with its license (Qt LGPL-3.0, FFmpeg LGPL-2.1+, libheif LGPL-3.0, ONNX Runtime MIT, WebGPU EP MIT + Dawn notices, SigLIP2 Apache-2.0, PDQ BSD, noise-c MIT, noise-java MIT, qrcodegen MIT, zxing-cpp Apache-2.0, Geist OFL-1.1, Lucide ISC, TinyEXIF MIT if used, YuNet MIT if used) and the LGPL source offer.
- Demo: `demo/demo.mp4` (60-90 s) + `demo/demo.gif` (12-15 s, < 8 MB) + `demo/poster.png`; no file or folder named "brag"; the real Bluetooth MAC never appears (QR blurred or a throwaway identity).
- Release only after: CI green on Windows/macOS/Linux + Android, security-audit report closed, gitleaks clean over full history, eval gates passed, the user's Android acceptance signed off.
- Merge to main locally with `git merge --no-ff`, authored and committed as Coflazo, no AI trailers, then push; `gh release create v2.0.0` with ZIP + APK + SHA-256 sums.

## Review Focus

1. A claim in the README that has no artifact behind it (a number from memory, a feature not shipped). Pinned in Task 4.
2. A demo frame that shows private data (real MAC, hostname, real personal photos). Pinned in Task 5.
3. A release asset that differs from what CI built (local rebuild uploaded). Pinned in Task 6.
4. A third-party license obligation missed in the ZIP (LGPL texts, Dawn notices, OFL font license). Pinned in Tasks 2 and 6.
5. A security-audit finding closed by assertion rather than by a test or a code change. Pinned in Task 1.

---

### Task 1: Security gates

- [ ] Run the Cloudflare `security-audit` skill in full-audit mode over `core/`, `engine/`, `desktop/`, `android/`, `tools/`, CI; run its validator CLI inside a Linux container (or a CI job) so the run completes (v1's run stayed `incomplete` on Windows). Inputs: docs/PROTOCOL.md, docs/SECURITY.md threat model.
- [ ] Run `cso` as a second opinion; dispatch `security-reviewer` and `silent-failure-hunter` agents over the whole branch.
- [ ] `santa-method`: two independent Opus reviewers on (a) the link handshake + pairing UX, (b) the removal transaction + vault; both must pass.
- [ ] Every finding: fixed with a test, or accepted with a written reason in `docs/SECURITY.md` "Known limitations". Fuzzers from Plan B Task 6 run 1 CPU-hour each, zero crashes.
- [ ] `gitleaks git --redact --log-opts="--all"` clean.
- [ ] Commit `docs(security): audit results and threat model`.

### Task 2: Documents

- [ ] `docs/SECURITY.md` (threat model STRIDE table, privacy model, offline proof, vault crypto, link crypto, reporting), `docs/MODEL.md` (nanoGPT-style: what goes in, the two-stage scoring math, training loop, eval protocol, numbers with CIs from `evaluation/report.json`, known failure modes, how to reproduce in one command), `docs/USER_GUIDE.md` (connect, learn, review, remove, restore, troubleshooting per phone family), `docs/PROTOCOL.md` (final), `THIRD_PARTY_NOTICES.md` + `licenses/` texts, `LICENSE` -> MIT, `docs/RELEASE_CHECKLIST.md` updated. Delete `PRODUCT.md` or rewrite it to v2 (impeccable product schema).
- [ ] Writing pass: stop-slop -> humanizer -> voice on every document.
- [ ] Commit `docs: v2 security, model, user guide and licenses`.

### Task 3: Figma exports and charts

- [ ] Export from the v2 Figma file (`mWYhEHB6Z1ogUt6g1xV6gM`): the pipeline diagram (generate with figma-generate-diagram) in light and dark SVG/PNG to `docs/assets/`.
- [ ] Real app screenshots (light and dark) of Onboarding, Learning, Review, Duplicates, Done via `gfxcapture` single frames from the test gallery; to `docs/assets/screens/`.
- [ ] `tools/eval_report.py` charts (taste AUC vs number of favorites with CI bands, near-dup precision/recall, ETA error over progress, items/s CPU vs GPU) rendered per the dataviz skill to `docs/assets/charts/` in light and dark.
- [ ] Commit `docs(assets): diagrams, real screenshots and evaluation charts`.

### Task 4: README

Structure (from the master plan): name + one line + demo GIF; what it does / for whom / exact offline claim; Highlights (5 measured bullets); Quick start; How it works (diagram); The model (math + eval table with CIs + failure modes); What it understands (media-kind table); Connecting a phone (USB vs QR + Bluetooth, measured speed matrix, 4 security bullets); Privacy and security; Compared to other tools (from `.superpowers/competitors-2026-10-07.md`, dated, sourced); Why shouldn't you use this?; Screens (light/dark `<picture>`); Configuration recommendations (measured matrix); Tech stack; Build, test, reproduce; FAQ; Roadmap (iOS companion over BLE, signed builds); Contributing; License; Acknowledgements.
- [ ] Draft; then stop-slop -> humanizer -> voice; then a claim audit: a table in the PR description mapping every number/claim to its artifact path (Review Focus 1); render check with playwright-mcp (headless) of the GitHub-flavoured markdown via `gh markdown` or a local renderer; GitHub description + topics updated with `gh repo edit`.
- [ ] Commit `docs: README for v2`.

### Task 5: Demo video

- [ ] Storyboard first (storyboard skill): 0-3 s chaotic swipe montage, 3-10 s "Storage full. 40,000 photos. Zero cloud." kinetic type, 10-70 s full real demo (onboarding asks the model, steps strike through, QR scan on the phone, previews stream in with ETA, learning curve, categories by reclaimable space, swipe review incl. a playing video, duplicate best pick, "Moved N to Trash, 0 bytes left your devices", restore from vault), 70-85 s proof numbers + CTA. Shots <= 5 s, captions burned in, one CTA.
- [ ] Capture: desktop with `ffmpeg -f lavfi -i gfxcapture=window_exe='^phone_memory_slider\.exe$':max_framerate=60,hwdownload,format=bgra -c:v libx264 -crf 16` on the test gallery; phone with `adb screenrecord` during the user's acceptance run (S9); pairing QR from a throwaway desktop identity or blurred.
- [ ] Edit with HyperFrames through the brag skill (pinned `npx hyperframes@<version>`, `HYPERFRAMES_NO_TELEMETRY=1`), product-launch-video for structure, video-use for cuts, media-use for a CC BY 4.0 / Pixabay music track + SFX (credited in `demo/CREDITS.md`). HyperFrames lint + check pass.
- [ ] Render `demo/demo.mp4` (1920x1080, 60 fps if the capture allows), `demo/demo.gif` via ffmpeg palettegen (12-15 s loop, < 8 MB), `demo/poster.png`; frame-by-frame privacy scan of every QR/text frame (Review Focus 2).
- [ ] User approval (human step), then commit `docs(demo): chaotic product demo from real footage`; remove `brag-output/` from HEAD in the same commit.

### Task 6: Release v2.0.0

- [ ] Version bump to 2.0.0 (CMake project version, Android versionName/versionCode).
- [ ] Merge `v2-link` into `v2` (if not already), then `git checkout main && git merge --no-ff v2`, verify `git log -1 --format='%an <%ae> | %cn <%ce>'` is Coflazo, push main.
- [ ] Wait for CI on main; download the CI-built artifacts (Windows/macOS/Linux ZIP + release APK + checksums) with `gh run download` (Review Focus 3); run `scripts/check-package.ps1` on the downloaded ZIPs.
- [ ] `gh release create v2.0.0` with those artifacts, `SHA256SUMS`, and release notes written from the README's Highlights + known limitations (humanizer + voice pass).
- [ ] Close the v1 release candidate branch notes (`release/v1.0.0-usb-offline`) by a final note in its README pointing to v2; do not delete it.
- [ ] Update the skillstack only if skills changed (they should not).
