# Proposal: Audiobookshelf adapter for self-hosted sync

**Status:** design proposal for public discussion. No code, no firmware image, no device test. It extends PR #95 without changing its protocol.

**Related:** issue #80, PR #95, and @davralin's comment on PR #95 asking for Audiobookshelf books with regular progress sync.

## Intent

PR #95 ships Navidrome first. Its coverage section marks audiobook progress as unmet in v1 and waits for an Audiobookshelf adapter. This document defines that adapter.

The adapter runs in the Rust sync server. The player keeps one generic sync client. Adding Audiobookshelf needs no firmware change and no reflash.

## Goals

- Download audiobooks from an Audiobookshelf library.
- Report listening position, duration, progress percentage and finished state back to Audiobookshelf.
- Read server progress at sync so a second device can resume the same book.
- Map Audiobookshelf library item IDs to the `server_id` used by PR #95.

## Non-goals

- No EPUB reader or read-along view on the player. Audiobookshelf's ebook support stays on the server.
- No OPDS client in firmware. The server reads Audiobookshelf's HTTP API.
- No playback session API in v1. Progress uses the media progress endpoints.
- No Storyteller alignment or media overlays.
- No change to the sync protocol, manifest schema or TLS rules in PR #95.

## API mapping

Audiobookshelf authenticates with a user API token in `Authorization: Bearer <token>`. The adapter never places the token in a URL, log or diagnostic output. This matches the TLS and token rules in PR #95 §5.6.

| Job | Audiobookshelf endpoint | Use in the sync protocol |
|---|---|---|
| List books | `GET /api/libraries/:id/items` | Build `/sync/manifest` entries |
| Read one book | `GET /api/items/:id?include=progress` | Fill metadata and current progress |
| Download audio | `GET /api/items/:id/download` | Feed the server side of `GET /sync/file` |
| Read progress | `GET /api/me/progress/:libraryItemId` | Server-to-player state |
| Write progress | `PATCH /api/me/progress/:libraryItemId` | Player-to-server state |
| Batch progress | `PATCH /api/me/progress/batch/update` | Offline sync after reconnect |

The official API docs are marked out of date. The adapter needs a host test against a live Audiobookshelf 2.x server before release.

## Identity

Audiobookshelf `libraryItemId` becomes `server_id` in `SYNC_IDENTITY`. The identity row records `entity_kind=book` and `provider=audiobookshelf`. Each audio file in the item gets a part identity from its file ID or, when the server exposes no ID, from the audio hash defined in PR #95 §5.7.17. The `idx` field orders parts and carries no identity, as PR #95 requires.

## Progress mapping

| Audiobookshelf field | Sync field | Rule |
|---|---|---|
| `currentTime` | audiobook resume position | Seconds into the book |
| `duration` | book duration | Seconds |
| `progress` | progress percentage | 0 to 1 |
| `isFinished` | finished flag | Maps to `AUDIOBOOK_FINISHED` |
| `finishedAt` | finished timestamp | Milliseconds since epoch, converted to the player's clock |
| `lastUpdate` | conflict timestamp | Newer value wins, using the PR #95 `server_time` rule |

Player-to-server sync sends one `PATCH` per changed book. A device that listened offline sends a batch update on reconnect. The adapter treats repeated `PATCH` calls with the same `currentTime` and `lastUpdate` as no-ops.

Server-to-player sync reads progress and writes it through the audiobook setters in PR #95 §5.7.2, under `audiobookdb.c`'s lock and after draining pending local checkpoints.

## Conflict handling

Audiobookshelf compares `lastUpdate`. The adapter keeps that rule. A stale local edit loses and the conflict record reports the loss, matching PR #95's position policy. The adapter does not start playback sessions in v1, so it avoids the session/progress divergence that dual writes would create.

## Phases

- This PR: design only, no code.
- Server PR: implement the adapter in the separate Rust server deliverable named in PR #95. Add host tests for auth, list, download, progress read and progress write.
- Device test: run after PR #95 Phase 1 ships the generic client. Verify download, resume and a second device reading the same book.

A future Stage 1 UI tile would use a 140×140 PNG derived from the official Audiobookshelf logo, matching the existing service icons in `assets/icons/`. No icon is part of this PR.

## Risks

- The Audiobookshelf API docs are out of date. Live-server tests gate the server PR.
- A token in a config file remains readable by root on the player. PR #95 states that limitation.
- Large books stress the download path. PR #95 already bounds size and verifies bytes before indexing.
- Audio frame hashing covers M4A/M4B and MP3 in PR #95. Other formats stay unmanaged until their byte ranges are defined.

## Checklist

- [x] Design document added
- [x] No runtime code or firmware image
- [x] API mapping and conflict rule stated
- [x] Gap from PR #95 closed in design
- [ ] Server PR implemented and tested against Audiobookshelf 2.x
- [ ] Device test after PR #95 Phase 1
