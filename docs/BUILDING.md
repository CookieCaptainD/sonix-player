# Building

This documents the build environment **as observed**, including failures. No
firmware image was built by this integration work; the validation described in
`docs/QA/TEST_REPORT.md` was run against images that already existed in the
local tree.

## Layout

* Repository root: `sonix-player/` (the project root).
* Sources: `sonix-player/sonix-player/src/`.
* Packer: `sonix-packer/sonix_firmware_packer.sh`.
* Device assets: `sonix-packer/assets/R1/`, `sonix-packer/assets/R3PII/`.
* V1 fork: separate repository; `sonix-packer/assets/V1/` only.

## Application build

`sonix-player/sonix-player/Makefile` targets:

| Target | Output | Notes |
|---|---|---|
| `make all` | host binary + compile commands | default |
| `make host` | `sonix_player_host` | SDL2 simulator; needs system `opusfile` and `wavpack` dev packages |
| `make target` | `sonix_player`, `sonix_launch` | cross-compiles with `rockbox-toolchain`, then `check-abi` and `strip-target` |
| `make check-abi` | — | refuses symbols newer than device glibc 2.22 |
| `make test-axpcharge` | test binary | charger state-machine test |
| `make libs-clean` | — | removes downloaded/built libraries |

There are **no** `r1`, `r3proii` or `v1` make targets. The same binary is built
once and the device is selected at runtime by `system-info.json` (see
`DEVICE_COMPATIBILITY.md`). The V1 fork adds `web-emu`, `run-web-emu`, `web-wasm`
and `web-wasm-bundle` targets.

Observed build attempt in the integration worktree (2026-11-22):

```
cp <original-tree>/*.tar.* sonix-player/
make host
```

Result: **FAILED**, exit 2. `pkg-config` could not find `opusfile` or `wavpack`
and `src/system/decode/opusdec.c:6` failed with `opusfile.h: Ficheiro ou pasta
inexistente`. The host build expects those libraries to be installed system-wide
(`libopusfile-dev`, `libwavpack-dev`); the vendored tarballs in the repository
are used for the **target** build only. A prebuilt `sonix_player_host` binary was
present in the original tree and was used for screenshot capture instead.

## Firmware packaging (existing process)

```
cd sonix-packer
# requires r1_original.upt and/or r3proii_original.upt next to the script
./sonix_firmware_packer.sh
```

The packer reads the `MODELS` table, skips models whose stock image is missing,
unpacks the ISO, swaps the player/launcher, overlays per-model assets, rewrites
the squashfs, rebuilds the OTA chunks and writes `r1.upt` / `r3proii.upt`. In the
primary local tree only `r1_original.upt` exists, so only `r1.upt` can be
produced there. The V1 fork uses its own packer plus `prepare_v1_stock.sh` and
`verify_upt.sh`, and requires the official V1 1.3 stock package.

Required tools observed: `7z`, `unsquashfs`, `mksquashfs`, `mkisofs`/`genisoimage`/
`xorrisofs`, `md5sum`, `split`, `cat`. The validator additionally uses
`python3`, `file`, `xxd` and `sha256sum`.

## Proposed per-device targets (not implemented)

To satisfy `make r1`, `make r3proii`, `make v1` and `make all` without duplicating
the application, a thin wrapper would need to:

1. build or reuse the single application binary;
2. select the correct asset tree (`assets/R1`, `assets/R3PII`, V1 `assets/V1`);
3. invoke the corresponding packer with the correct stock reference;
4. run `tools/validate_upt.py --expect-device <device>` and fail the build on a
   structural error;
5. emit the image to a device-specific path with its SHA-256.

This is a proposal only. It has not been implemented, so those commands do not
work in this worktree.

## Validation (implemented and run)

```
python3 tools/validate_upt.py <image>.upt --expect-device r1|r3proii|v1
```

The tool independently opens the ISO, verifies metadata, the per-chunk MD5
chain, the manifest and the reassembled component MD5s, then reads
`device-name` from the rootfs. Exit 0 means structural checks passed — **not**
that the image is safe to flash. See `docs/firmware/FLASHABILITY_MATRIX.md`.
