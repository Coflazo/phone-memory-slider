# Phone Memory Slider user guide

## Before you start

Use a data-capable USB cable and keep a separate backup of important media. On Windows, unlock the phone and approve the computer; Android may also require **File Transfer / MTP** mode. On macOS or Linux, mount or export the phone's DCIM folder and choose it in the app.

The app needs enough free computer storage for the files you decide to remove. A useful rule is the queued batch size plus 10% headroom.

## Connect and scan

1. Connect and unlock the phone.
2. Open Phone Memory Slider. Choose the detected cable source, or choose a local/mounted DCIM folder.
3. Review the source description. The app can only analyze what the operating system exposes.
4. If the scan reports visible Favorites, choose **Analyze now** to learn from them automatically. Otherwise select 20–50 photos you strongly want to keep; these local examples stand in for album metadata the cable protocol hides.
5. Start analysis. Previews, hashes, features, and model training remain on the computer.

Twenty keeps enable personal similarity. Thirty to fifty varied examples usually give the model a more useful picture of your taste. Without 20 examples, duplicate, quality, and storage rules still work but personalization stays neutral.

## Review

- Swipe or press **Left** to queue an item for removal.
- Swipe or press **Right** to keep it.
- Choose **Undo** or press **Z** before confirmation begins.
- Videos play inside the review card when the platform codec supports them.
- Read the reason tag as evidence, not a prediction certainty. The app never deletes automatically.

## Confirm and recover

The summary shows the queued count and estimated space. **Encrypt, verify, remove** processes one file at a time and refuses any item whose bytes changed after review:

1. Copy the source locally.
2. Encrypt it into the recovery vault with AES-256-GCM.
3. Decrypt it into a verification file.
4. Compare size and SHA-256 with the source.
5. Remove the source only after every prior step succeeds.

The vault location is shown by the app. Do not delete its key file if you need to recover payloads.

## Troubleshooting

- **No phone appears:** try another data cable/USB port, unlock the phone, approve trust, and select File Transfer/MTP. Some devices expose DCIM only through vendor drivers.
- **Only some photos appear:** cloud-only, optimized-storage, secure-folder, or app-private media may not be exposed over USB. Download/export it locally first.
- **Delete is denied:** the device may expose read-only media. Choose a folder copy or use the phone's gallery to remove the reviewed items.
- **A preview is blank:** the phone may not provide a thumbnail or the desktop may lack that codec. The original is not removed unless the vault transaction succeeds.
- **Analysis is slow:** full-file SHA-256 is I/O-bound. Keep the phone awake, use a direct USB port, and avoid a heavily loaded hub.
- **Video will not play:** install an OS codec for that format or review the file on the phone before deciding.
