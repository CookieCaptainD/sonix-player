# Proposed PR body (draft — not published)

**Title:** `Audit and firmware-safety package for three-device consolidation (R1, R3 Pro II, V1)`

**Target:** `Tartarus6/sonix-player` — branch `main`
**Source:** `CookieCaptainD/sonix-player` — branch `integration/three-device`

> This contribution is an **audit and safety package**, not a finished three-device
> integration. It deliberately does not claim V1 support, per-device builds, CI,
> or flashable firmware. Please read the classification table before reviewing.

## Summary

This branch preserves the existing working tree and adds:

* a full inventory of the local repository, upstream and the TempoTec V1 fork,
  with commit SHAs and feature provenance;
* a neutral comparison of competing implementations and an item-by-item decision
  for every local improvement absent from the V1 fork;
* an independent, read-only `.upt` validator that checks ISO structure, OTA
  metadata, MD5 chains, manifests, component MD5s and rootfs device identity;
  wrong-device, truncated and corrupted inputs are rejected in executed tests;
* per-device flashability documentation, including an independent safety review;
* one genuine host-simulator screenshot and a complete test report.

## Device status

| Device | Build in this branch | Flashability |
|---|---|---|
| HiBy R1 | no new image built | NOT VERIFIED FOR FLASHING |
| HiBy R3 Pro II | no new image built | REJECTED (mandatory compatibility metadata absent) |
| TempoTec V1 | no V1 code or image merged | NOT VERIFIED FOR FLASHING |

No image in this contribution is safe to flash. No signature scheme was found;
recovery procedures are documented from source only and were not tested.

## Deliberately out of scope here

V1 code integration, `make r1`/`r3proii`/`v1` targets, firmware CI, OTA fixes,
three-device screenshots and physical-device validation are **not implemented**.
They are documented as required work in the report. Do not merge expecting them.

## Documentation

Complete engineering report (Markdown):
[docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.md](docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.md)

Complete engineering report (PDF, all evidence):
[docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.pdf](docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.pdf)

Key documents:
[flashability matrix](docs/firmware/FLASHABILITY_MATRIX.md) ·
[flashing safety](docs/firmware/FLASHING_SAFETY.md) ·
[recovery guides](docs/firmware/RECOVERY_GUIDES.md) ·
[test report](docs/QA/TEST_REPORT.md) ·
[UI review](docs/QA/UI_REVIEW.md) ·
[implementation comparison](docs/audit/IMPLEMENTATION_COMPARISON.md)

## Validation

`python3 tools/validate_upt.py <image>.upt --expect-device r1|r3proii|v1`
Exit 0 means structural checks passed — explicitly **not** flashability approval.
Reports: `docs/QA/validation/`.

## Publication and flashing

Nothing has been pushed or released. Opening this PR and any physical flashing
require separate explicit authorisation from the repository owner.
