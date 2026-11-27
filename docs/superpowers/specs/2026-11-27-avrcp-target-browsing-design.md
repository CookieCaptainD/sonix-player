# AVRCP target browsing design

Date: 2026-11-27
Base: Tartarus6/sonix-player v1.3.3, merge commit `c54f8e4`

## Intent

A car head unit that pairs over Bluetooth acts as an AVRCP controller. v1.3.3 already answers two of its needs: `btplayer.c` registers an MPRIS player with bluez, so the car reads track title, artist, album and playback state, and sends play, pause and skip. The head unit cannot ask for the library. It gets no folder list, no track list, and no way to play a track it found in either.

This work adds the missing half: the AVRCP target browsing role. The car can walk the scanned music library, read names, move up one level, and start playback of a selected track.

## What v1.3.3 already gives us

- `dbuslite.c`: a hand-written D-Bus client. It covers the system bus messages this player needs and avoids linking libdbus, which is absent from the toolchain sysroot.
- `btplayer.c`: registers `org.mpris.MediaPlayer2.Player` with bluez, keeps metadata and status in step with playback, and emits `PropertiesChanged`. It also filters duplicate transport commands that arrive twice, once as a key event and once as a method call.
- `btstack.c`: one D-Bus connection and one Bluetooth worker. Signals arrive on the reader thread, update a cache, and ring a callback. Calls run on the worker.
- `btreceiver.c`: receiver mode, the opposite role, where the player acts as a speaker for a phone.

No file in v1.3.3 registers a media browser or answers `GetFolderItems`, `ChangePath` or `PlayItem`. The new work sits beside `btplayer`, not inside it.

## Goals

1. A head unit can read a folder tree built from the library the player already scanned.
2. It can open a folder, move back one level, and play a track chosen from a folder.
3. Names in the car match names on the player. No separate metadata source.
4. The feature starts on. A file on /usr/data turns it off without reflashing.
5. Stock firmware keeps its current behavior. With the toggle off, or without the patched daemon, the player behaves exactly as v1.3.3.
6. The packed image stays inside the 48 MiB ceiling already used by the firmware packer gate.

## Non-goals

- No change to the library scanner, the tag reader, or the database.
- No browsing of Tidal, Qobuz, radio, podcasts or network shares over AVRCP. Local scanned media only.
- No cover art over the browsing channel. The PDU payload budget and the image ceiling both argue against it.
- No new on-device CI job. CI runs host harnesses. Device behavior comes from the user test reported below.
- No claim that every head unit works. AVRCP browsing support and rendering vary by dashboard firmware.
- No change to receiver mode. A phone browsing the player is a different role from a car browsing it.

## Architecture

Four pieces carry the feature. Each has one job.

**Player-side browser, `btbrowser.c`.** The scanned library supplies tracks, artists and albums. A provider walks those handles and writes D-Bus replies. The filesystem tree starts at `/org/mpris/MediaPlayer2/Filesystem` with `Artists`, `Albums` and `Tracks` folders. Nested artist and album folders generate child folders from library rows. The daemon keeps a 512-entry LRU cache that maps `xesam:url` values to stable 64-bit UIDs. `btbrowser.c` keeps a separate 4096-entry table of folder object paths.

**D-Bus bridge, `dbuslite.c` plus `btplayer.c`.** The player registers with bluez once. `btbrowser.c` answers the browse calls on that connection and returns false for messages it does not own, so the MPRIS interfaces still reach `btplayer.c`. Replies use the writer and alignment rules already in `dbuslite.c`. The target side calls MPRIS through `media-browse.c` with a 3000 ms deadline, one retry on timeout, and an exact signature check on every reply.

**AVRCP PDU layer, in `tools/bluez-browse/modules/`.** Stock BlueZ 5.54 knows how to ask for folder items; it does not answer them. The patch adds `avrcp-tg-browse.c` for the PDU codecs, `media-browse.c` for the MPRIS bridge, `tg-pending.c` for transaction state, and `avctp-defer.c` for replies that must leave after the MPRIS calls finish. The target answers `SetBrowsedPlayer` (0x70), `GetFolderItems` (0x71), `ChangePath` (0x72), `GetItemAttributes` (0x73), `PlayItem` (0x74) and `GetTotalItems` (0x75). It serves the VFS scope and clamps browse payloads to 505 bytes.

**Playback bridge.** `PlayItem` resolves a UID to a real file path through the provider and starts it through the same playback entry point the GUI uses. Queue, gapless and output routing stay in one place.

**Fallback, `bluetoothd_fallback.c`.** Bring-up tries `/usr/libexec/bluetooth/browse/bluetoothd` first, then `/usr/libexec/bluetooth/bluetoothd`. A disable flag file turns the patched daemon off. Two attempts share one deadline. The patched daemon must be killed before the stock one starts, because two bluetoothds fighting over `org.bluez` leave the radio worse off than no daemon.

**Disable flag.** Browsing starts on. Creating `/usr/data/disable_bt_browse` selects the stock daemon on the next start. No settings UI exists. The v1.3.3 MPRIS metadata path stays in place either way.

## Data flow

```text
head unit --AVRCP/AVCTP--> patched bluetoothd --D-Bus--> browser bridge
browser bridge --UID lookup--> browse tree --answer--> browser bridge
browser bridge --D-Bus--> patched bluetoothd --AVRCP/AVCTP--> head unit
PlayItem --UID--> file --> existing playback entry point --> DAC / A2DP
```

## Tradeoffs

**Opt-in or default on.** Default on would reach every user without a setting, but it would replace the Bluetooth daemon for people whose head units never browse and for people who pair only headphones. Opt-in keeps the stock path as the default. The cost is one setting and a user who has to find it.

**Patched bluetoothd.** A patched daemon is the only way to answer the browsing PDUs with the firmware's BlueZ version. Replacing a system daemon carries risk: pairing and codec behavior can differ. The stock binary stays available, and seven fallback cases cover the transitions.

**Stock fallback.** On stock firmware the feature does not appear. That is deliberate. A half-registered browser that answers some commands and times out on others is worse than no browser.

**48 MiB image ceiling.** The v1.3.3 R1 image is 44,300,288 bytes. The packer gate refuses anything above 48 MiB. The patch adds no assets and sends no covers, so the image stays inside the gate. Much of the headroom goes to the patched daemon.

**No on-device CI.** Host harnesses check PDU bytes, D-Bus messages, AVCTP framing, pending requests and fallback. They cannot check a real dashboard. One user test on a real car unit confirms the flow. Head-unit variability stays a known limit.

**Dashboard variability.** Menus, UID handling and folder depth differ between cars. One head unit passing does not prove another will. The disable flag lets one car return to stock Bluetooth without a new image.

## Test gates

All gates run before merge. Counts below are the results recorded by the implementing session.

| Gate | Cases | Result | What it covers |
|---|---:|---|---|
| PDU conformance | 189 | 189 pass | PDU encodings, UID stability, status codes, folder counts |
| D-Bus | 15 | 15 pass | Registration, method signatures, reply types, signals |
| AVCTP | 14 | 14 pass | Framing, continuation, retransmission, close on disconnect |
| Pending requests | 12 | 12 pass | Serials, timeouts, cancel on disconnect, no duplicate replies |
| Fallback | 7 | 7 pass | Toggle off, stock daemon, empty library, daemon restart, mid-browse disconnect |
| App harness | full flow | pass | Folder open, track list, back, play item from the UI side |

Device check, reported by the user on a real car head unit: folders show, track names show, back navigation works, and clicking a track plays it.

## Journey: seven review rounds

The work went through seven review rounds. Each entry names the issue raised and the change it produced.

1. **Role boundary.** The first round asked where browsing belongs. Putting it inside `btplayer.c` would mix metadata polling with request state. The change kept browsing in its own module beside the player and left the MPRIS path alone.

2. **Enablement.** The second round challenged the first disable plan. A settings toggle would have added UI work and left no recovery path when the UI could not load. The change used a file on the writable partition as the off switch.

3. **Daemon replacement.** The third round focused on the patched bluetoothd. Pairing and codec regressions would hit the whole Bluetooth stack. The change kept the stock binary as fallback, kept the stock binary as the fallback, and added the fallback gate.

4. **PDU correctness.** The fourth round ran the PDU vectors. UID handling and folder-count encoding needed work before a head unit could hold an ID across calls. The change fixed those encodings and brought the suite to 189 passing cases.

5. **Transport and D-Bus framing.** The fifth round checked the two wires. Alignment errors on D-Bus and continuation errors on AVCTP both close the connection rather than produce a wrong value. The change tightened the writers and the request serializer; 15 D-Bus cases and 14 AVCTP cases pass.

6. **Pending state.** The sixth round asked what happens when a head unit disconnects mid-request. Late replies and duplicate answers could corrupt the next browse. The change added serials, deadlines, and cancel on disconnect; 12 pending cases pass.

7. **Packaging and proof.** The seventh round checked what we can actually claim. The R1 image must stay under 48 MiB, CI has no device, and dashboards differ. The change added the packer gate check, kept the app harness for the flow, and recorded the user's car result as a device report instead of a CI result.

## Related release material

The release notes and the comparison with v1.3.3 live in `pr-work/PR-BODY.md`. The evidence captures, including the harness summary and the architecture diagram, live in `docs/pr-assets/`.
