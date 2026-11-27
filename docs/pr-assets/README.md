# PR evidence

Every PNG here is a render of captured text or a drawn diagram. None is a car UI screenshot.

| File | What it is |
|---|---|
| `harness-summary.txt` | Output of `harness_report.py`. It reads `results.json` and computes the totals. |
| `harness-summary.png` | Render of `harness-summary.txt` with ImageMagick. |
| `release-summary.txt` | Output of `release_summary.py`, fetched from the GitHub API for tag `v1.3.3`. |
| `release-summary.png` | Render of `release-summary.txt`. |
| `review-journey.txt` | The seven review rounds, captured from the design doc by `review_excerpt.sh`. |
| `review-journey.png` | Render of `review-journey.txt`. |
| `host-harness.txt` | Real output of six module harnesses from `tools/bluez-browse/tests` and the fallback harness from `sonix-player/tests/harness`, compiled by `capture_host_harness.sh`, including the app harness. |
| `host-harness.png` | Render of `host-harness.txt`. |
| `architecture.dot` | Canonical graph description of the components and edges. |
| `architecture.png` | The same nodes and edges drawn with ImageMagick. |
| `results.json` | Gate counts recorded by the implementing session. |

## Tool availability on the host

- ImageMagick: available as `magick` and `convert`, version 7.1.2-36. Used for all PNG renders.
- Graphviz: `dot`, `neato`, `sfdp`, `twopi` and `circo` are not installed. The DOT source is kept, and `render_architecture.sh` draws the same graph with ImageMagick instead.

## Provenance

The gate counts in `results.json` come from the implementing session. `harness_report.py` does not invent results; it reads that file and computes the total. The device lines are the user's report of a real car test, not CI output and not a screenshot. `release_summary.py` fetches live metadata and does not hard-code release values.

## Regenerate

```sh
python3 harness_report.py > harness-summary.txt
python3 release_summary.py > release-summary.txt
./review_excerpt.sh
./capture_host_harness.sh
./render_evidence.sh
./render_architecture.sh
```
