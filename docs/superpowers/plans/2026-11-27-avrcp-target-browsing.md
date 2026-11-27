# AVRCP target browsing implementation plan

Date: 2026-11-27
Base: Tartarus6/sonix-player v1.3.3, merge commit `c54f8e4`
Design: `docs/superpowers/specs/2026-11-27-avrcp-target-browsing-design.md`

## Intent of this plan

Turn the design into work that a reviewer can check. The plan keeps the stock Bluetooth path intact, adds browsing behind a setting, and names the gates that must pass before the PR is ready.

## Starting point

The branch starts at the v1.3.3 merge commit. There is no earlier implementation of target browsing in the tree to diff against. Comparison with the release happens at feature level, not line level.

v1.3.3 already has the pieces the feature needs to sit beside:

- one D-Bus connection in `btstack.c` and a hand-written client in `dbuslite.c`;
- an MPRIS player in `btplayer.c` that answers metadata and transport commands;
- a library scan that already produces folders, albums and tracks;
- a packer gate that refuses images above 48 MiB.

## Work order

### 1. Baseline and boundary

Record the v1.3.3 tree state and confirm the starting commit. Agree that `btplayer.c` keeps metadata and transport, and that browsing gets its own module. No behavior change on this step.

Exit: branch at `c54f8e4`, no source changes yet.

### 2. Browse tree

Build `btbrowser.c` over the library handles already owned by `btplayer.c`. Expose `/org/mpris/MediaPlayer2/Filesystem`, a flat `Tracks` folder, and item paths below it. List tracks, artists and albums. Cache 512 MPRIS track ids as stable 64-bit UIDs. Read names from the same tag data the GUI uses.

Exit: a head unit can ask for a folder and get a stable list of child UIDs and names.

### 3. D-Bus bridge

Answer browse calls on the existing connection. Let `btbrowser.c` return false for messages it does not own so `btplayer.c` keeps the MPRIS interfaces. Reuse the writer and alignment rules in `dbuslite.c`. On the daemon side, call MPRIS through `media-browse.c` with a 3000 ms deadline, one retry and an exact signature check.

Exit: 15 D-Bus cases pass.

### 4. PDU layer in the patched daemon

Add `avrcp-tg-browse.c` under `tools/bluez-browse/modules/`. Encode `SetBrowsedPlayer`, `GetFolderItems`, `ChangePath`, `GetItemAttributes`, `PlayItem` and `GetTotalItems`. Serve the VFS scope. Clamp browse payloads to 505 bytes. Keep the stock binary in the image for fallback.

Exit: 189 PDU cases pass, including UID stability and folder counts.

### 5. AVCTP framing and pending state

Add `avctp-defer.c` to hold a frame until the MPRIS calls finish. Add `tg-pending.c` to match replies to the current transaction and drop stale or disconnected players. Handle continuation and retransmission without duplicate answers. Cancel a pending request when the controller disconnects.

Exit: 14 AVCTP cases and 12 pending cases pass.

### 6. Playback bridge, daemon fallback and toggle

Resolve a played UID to a file and start it through the existing playback entry point. Add `bluetoothd_fallback.c`: try the patched daemon at `/usr/libexec/bluetooth/browse/bluetoothd`, then the stock one at `/usr/libexec/bluetooth/bluetoothd`, with a disable flag and a shared deadline. Kill the patched daemon before starting stock. Browsing starts on. Create `/usr/data/disable_bt_browse` to select the stock daemon on the next start. No settings UI is added.

Exit: 7 fallback cases pass. App harness completes folder open, back and play item.

### 7. Packaging and evidence

Pack the image and check it against the 48 MiB gate. Capture harness output and log excerpts as PNG files under `docs/pr-assets/`. Write the PR body with the release comparison.

Exit: packer gate passes, evidence files exist, PR body reads cleanly.

### 8. Device check

Install on a device, enable the setting, pair with a head unit that supports browsing, and walk the flow. Record the result as a device report. CI does not run this step.

Exit: user confirms folders, track names, back navigation and click to play on a real car unit.

## Test gates

| Gate | Count | Gate rule |
|---|---:|---|
| PDU conformance | 189 | all pass |
| D-Bus | 15 | all pass |
| AVCTP | 14 | all pass |
| Pending requests | 12 | all pass |
| Fallback | 7 | all pass |
| App harness | full flow | pass |
| Packer gate | 1 | image at or below 48 MiB |
| Device | 1 flow | user confirmed |

Recorded results match every gate above. See `docs/pr-assets/harness-summary.txt` and `docs/pr-assets/harness-summary.png`. Real compiled module harness output is in `docs/pr-assets/host-harness.txt` and its PNG render.

## Order of risk

1. Daemon replacement. Highest risk, lowest tolerance. Fallback gate covers it.
2. Pending state and disconnect. A late reply corrupts the next browse. Twelve cases cover it.
3. PDU and D-Bus framing. Wrong alignment closes the connection. Twenty-nine cases cover both wires.
4. Image size. The gate fails the build before a bad image ships.
5. Device variability. One head unit tested. The setting keeps the blast radius small.

## What we do not do

- No default-on rollout in this PR.
- No patch applied when the toggle is off.
- No cover art over the browsing channel.
- No claim of support for every car dashboard.
- No fabricated device screenshots. Evidence is harness output, log excerpts and a drawn architecture diagram. The car result is a written user report.

## Verification before merge

Run every host gate on the final tree. Pack both R1 and R3 Pro II images and check the gate output. Confirm the PR body states the counts, the device result, the tradeoffs and the v1.3.3 comparison. Do not commit on this step.
