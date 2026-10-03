# Phone Memory Slider beta user guide

## Before you start

You need an Android 11 or newer phone, a desktop on the same private network (USB tethering also works), the Android companion APK, and the desktop package. Keep both apps open during review. iPhone import is not part of this beta.

## Pair and scan

1. Open the Android companion and grant photo/video access. Selected-photo access works, but only selected items can be reviewed.
2. Tap **Start local pairing**. The phone shows one or more private-network addresses and a six-digit code.
3. Open the desktop app, enter an address such as `192.168.1.24:41820`, enter the code, and choose **Verify and connect**.
4. Leave the phone companion open. The desktop resumes an incomplete catalog when possible; if the gallery changed, it safely restarts that catalog revision.
5. Visual analysis runs locally on the desktop. The personal preference signal activates after at least 20 accessible favorites; before that, quality, similarity, and storage signals drive the order.

## Review

- Swipe or press **Left** to queue an item for recoverable trash.
- Swipe or press **Right** to keep it.
- Choose **Undo** or press **Z** before confirmation begins.
- Videos download in bounded chunks to a temporary local cache, start muted, and offer play/pause and sound controls. The cache is removed when the card changes or the app exits.
- The reason tag is an explanation, not a probability. The app never deletes automatically.

## Confirm on Android

After the last card, review the queued count and storage estimate. Choose **Continue on phone**. Android rechecks that every item still exists and is not a favorite, then opens its own recoverable system-trash prompt. Cancelling that prompt keeps the library unchanged. Trash retention duration is controlled by Android and the gallery provider.

## Troubleshooting

- **Pairing fails:** confirm both devices use the same private network, copy the current code exactly, and allow the app through the Windows private-network firewall prompt.
- **No media:** check Android photo/video permissions. Selected access intentionally limits the catalog.
- **Scan restarts:** the phone gallery changed while a revision-bound catalog was transferring; restart is the safe behavior.
- **Video cannot cache:** free local disk space and retry. Originals are not retained by default; only the current video is cached temporarily.
- **Connection pauses:** reopen the phone app and start a new session. Completed local analysis stays in the desktop catalog.
