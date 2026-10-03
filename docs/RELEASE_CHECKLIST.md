# Beta release checklist

## Automated gates

- [ ] Repository consistency and local-only endpoint check passes.
- [ ] C++ Debug and Release builds pass with warnings treated as errors.
- [ ] CTest passes in Debug and Release.
- [ ] The 250,000-item benchmark stays below two seconds and 1.5 GB estimated working memory.
- [ ] Android unit tests and `assembleDebug` pass with SDK 36.
- [ ] Windows portable package contains the executable, Qt runtime, license, privacy guide, and Geist notice.
- [ ] macOS and Linux CI packages are produced.
- [ ] CI publishes the Android APK artifact.

## Hardware acceptance (must be performed on a real Android phone)

- [ ] Full and selected-gallery permission modes catalog only authorized media.
- [ ] Correct code pairs; wrong code and a changed certificate fail without exposing catalog data.
- [ ] Disconnect during scan resumes without mixing catalog revisions.
- [ ] A gallery change during paging triggers a clean restart.
- [ ] Photo preview and at least one H.264/MP4 video play on Windows.
- [ ] A favorite added after desktop review is excluded by the phone recheck.
- [ ] Android cancellation leaves every queued item in the gallery.
- [ ] Approved items appear in the gallery provider's recoverable trash.
- [ ] Airplane mode with local Wi-Fi or USB tethering still works; public/internet addresses are rejected.

Do not describe a build as production-ready until signing, installer reputation, accessibility, and the complete hardware matrix have passed. This repository targets a local beta.
