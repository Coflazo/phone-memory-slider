# Release checklist

Do not describe a build as production-ready until every mandatory gate below passes for that exact commit and artifact.

## Automated gates

- [ ] Repository consistency and zero-egress source check pass.
- [ ] Packaged ZIP excludes network-information, TLS, and QML-debug TCP plugins.
- [ ] C++ Release build completes with warnings treated as errors.
- [ ] Every CTest target passes on Windows, macOS, and Linux.
- [ ] Windows GUI smoke launch exits cleanly.
- [ ] Recovery-vault multi-chunk round trip and tamper rejection pass.
- [ ] Folder end-to-end test proves preview, ranking, encrypted recovery, and source removal.
- [ ] 250,000-item core benchmark remains within the documented threshold.
- [ ] Evaluation report is reproducible and README values match the committed JSON.
- [ ] Packages contain the executable, required Qt/OpenSSL runtimes, license, privacy guide, and third-party notices.
- [ ] Release artifacts have checksums and provenance metadata.

## Offline and security acceptance

- [ ] Run the packaged app behind a deny-all outbound firewall rule; every supported flow still works.
- [ ] Capture traffic for startup, scan, analysis, review, delete, recovery, and shutdown; observe no attempted egress.
- [ ] Review imports/dependencies for telemetry, update, crash-reporting, and network code.
- [ ] Confirm malformed/untrusted device names, paths, sizes, and content cannot escape bounded local storage.
- [ ] Confirm an altered vault nonce, ciphertext, tag, digest, or size cannot authorize source deletion.
- [ ] Replace media after review and confirm the digest-bound transaction retains the changed source.
- [ ] Archive two different files with the same asset ID and confirm both recovery payloads remain restorable.
- [ ] Confirm logs and errors do not disclose media content or stable device identifiers.

## Physical Windows device matrix

- [ ] Current Android phone over MTP: catalog, photo preview, video playback, copy, and delete.
- [ ] Current iPhone after Trust approval: DCIM catalog, preview, copy, and permitted delete behavior.
- [ ] At least one older Android device and one alternate OEM/driver.
- [ ] Cable disconnect during catalog, preview, hashing, vault copy, and delete fails safely.
- [ ] Locked/revoked-trust phone exposes no media and produces an actionable message.
- [ ] Cloud-only and secure-folder exclusions are accurately disclosed.
- [ ] Large files above 5 GB hash and enter the recovery transaction without whole-file memory use; previews above the documented 2 GiB cache ceiling fail safely.

## UX and accessibility acceptance

- [ ] Complete flow works with keyboard only.
- [ ] Screen-reader names and focus order are correct.
- [ ] Text and interactive controls meet WCAG AA contrast and target-size requirements.
- [ ] Reduced-motion mode removes spatial motion without hiding state changes.
- [ ] Long names, large counts, missing thumbnails, unsupported video, and narrow windows degrade cleanly.

## Distribution

- [ ] Windows executable and installer are signed with the Coflazo release identity.
- [ ] macOS app is signed, hardened, and notarized.
- [ ] Linux package metadata and dependencies are validated on supported distributions.
- [ ] A clean virtual machine can install, run offline, and uninstall without leftovers outside documented local data.
- [ ] Tag, release notes, Figma link, demo, checksum, and source commit agree.
