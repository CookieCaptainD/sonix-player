# Remaining EPUB Script Fonts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every chapter of *Around the World in 28 Languages* render without `▯` by adding the missing script faces to the EPUB reader's fallback chain.

**Architecture:** The reader already chains script fonts behind Bookerly through `lv_font_t::fallback` (PR #269). This plan adds six Noto Sans script families (regular and bold) to the firmware, reuses the existing MiSans faces for Simplified Chinese, and extends `SCRIPT_FACES[]` in `src/gui/ebook/ebookfonts.c`. No shaping or bidi is added.

**Tech Stack:** C (LVGL 9.6 + FreeType), Python 3 + fontconfig for the coverage check, shell for downloading fonts and packing the R1 image.

**Spec:** `docs/superpowers/specs/2026-11-16-epub-script-fonts-design.md`

## Global Constraints

- Base branch: `fix/epub-korean-font-fallback` (PR #269). Do not work on `main` until #269 merges.
- Work only in the isolated worktree `/tmp/opencode/sonix-epub-scripts`, branch `feat/epub-script-fonts`.
- Fonts are SIL OFL 1.1; ship `OFL-NotoSans.txt` beside them.
- Add fonts to both `sonix-packer/assets/R1/...` and `sonix-packer/assets/R3PII/...`. Leave `sonix-player/assets/fonts` untouched (the EPUB reader does not read it).
- Do not produce or flash an `r3proii.upt`. The device under test is the R1.
- No shaping/bidi claim: the delivery is glyph coverage only.

## Review Focus

- **Assamese/Bengali-style missing punctuation:** a script font may have letters but not its danda/marks; the coverage checker must count punctuation and combining marks, not only letters. Covered by Task 1 and Task 2.
- **Bold headings in a new script:** the bold face must cover the same characters as the regular one, or headings fall through to another script. Covered by Task 2's per-face check.
- **Mixed Han scripts:** Chinese must use MiSans and Japanese must stay Rodin. Covered by Task 3's chain order and Task 4's chapter checks.
- **Partly installed image / older firmware:** a missing script file must skip that script without breaking the reader. Covered by Task 3's `access()`-based loader review and Task 4's removal check.
- **Large reader size on a small panel:** new fonts have different metrics and may overflow a column. Covered by Task 4's simulator page-through at minimum and maximum size.

---

### Task 1: Coverage checker (failing test first)

**Files:**
- Create: `sonix-player/tools/epub_font_coverage.py`
- Commit: `docs/superpowers/specs/2026-11-16-epub-script-fonts-design.md`

**Interfaces:**
- Produces: `python3 sonix-player/tools/epub_font_coverage.py --epub <file.epub> --fonts <font-dir>`; exits `0` when every non-ASCII, non-whitespace character in the EPUB is covered by at least one font file found recursively under `<font-dir>`, `1` otherwise.

- [ ] **Step 1: Write the coverage checker**

The script must:

1. Require `--epub` and `--fonts`; fail clearly if `fc-query` is not on `PATH`.
2. Open the EPUB as a ZIP and read every member ending in `.xhtml`, `.html` or `.htm`.
3. Extract visible text with `html.parser.HTMLParser` (skip `<script>` and `<style>` content) and `html.unescape`.
4. For each file, label the chapter with `re.search(r"Universal Declaration of Human Rights - (.+)", text)` (fall back to the file name).
5. Collect `{ord(c) for c in text if ord(c) > 0x7f and not c.isspace()}`.
6. Build one coverage set: for every `*.ttf`, `*.otf`, `*.ttc` under `--fonts` (recursive), run `fc-query -f %{charset} <file>` and parse both single codepoints and `start-end` ranges.
7. Print one line per chapter with missing characters:

```
<label>: <n> missing  U+XXXX U+YYYY ...
```

Include every missing codepoint; chapters with none print `<label>: ok`.
8. Print `N chapters with missing glyphs` and exit `1` when `N > 0`; otherwise print `all chapters covered` and exit `0`.

- [ ] **Step 2: Run it against the current fonts and confirm it fails**

The checker reports the union of every font under `--fonts`. Point it at the faces the reader actually chains today (Bookerly plus the PR #269 scripts), not the whole firmware folder, or the UI-only MiSans files would mask the Chinese gap:

```bash
cd /tmp/opencode/sonix-epub-scripts
fonts=sonix-packer/assets/R1/usr/resource/sonix/fonts
rm -rf /tmp/ebook-chain-before; mkdir -p /tmp/ebook-chain-before
cp "$fonts/ebook/Bookerly-Regular.ttf" \
   "$fonts/korean.otf" "$fonts/korean-bold.otf" \
   "$fonts/japanese.otf" "$fonts/japanese-bold.otf" \
   "$fonts/thai.otf" "$fonts/thai-bold.otf" \
   "$fonts/arabic.otf" "$fonts/arabic-bold.otf" \
   /tmp/ebook-chain-before/
python3 sonix-player/tools/epub_font_coverage.py \
  --epub "/home/diogo/Área de trabalho/Around the World in 28 Languages.epub" \
  --fonts /tmp/ebook-chain-before
```

Expected: exit `1`, with exactly these labels and counts (order may differ):

```
Chinese, Mandarin (Simplified): 128 missing
Tamil: 42 missing
Nepali: 65 missing
Hindi: 72 missing
Marathi: 64 missing
Malayalam: 57 missing
Panjabi, Eastern: 58 missing
Amharic: 164 missing
Hebrew: 27 missing
9 chapters with missing glyphs
```

All other chapters print `ok`.

- [ ] **Step 3: Commit**

```bash
git add sonix-player/tools/epub_font_coverage.py docs/superpowers/specs/2026-11-16-epub-script-fonts-design.md
git commit -m "ebook: add EPUB font coverage checker and script fonts design"
```

---

### Task 2: Add the Noto script faces

**Files:**
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/devanagari.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/devanagari-bold.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/tamil.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/tamil-bold.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/malayalam.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/malayalam-bold.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/gurmukhi.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/gurmukhi-bold.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/ethiopic.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/ethiopic-bold.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/hebrew.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/hebrew-bold.ttf`
- Create: `sonix-packer/assets/R1/usr/resource/sonix/fonts/OFL-NotoSans.txt`
- Create: the same thirteen files under `sonix-packer/assets/R3PII/usr/resource/sonix/fonts/`

**Interfaces:**
- Produces: font files named exactly as listed; Task 3's `SCRIPT_FACES[]` entries depend on these names.

- [ ] **Step 1: Download the hinted static Noto Sans faces**

```bash
set -euo pipefail
src=/tmp/noto-script-fonts
rm -rf "$src"; mkdir -p "$src"; cd "$src"
base=https://raw.githubusercontent.com/notofonts/notofonts.github.io/main/fonts
for family in Devanagari Tamil Malayalam Gurmukhi Ethiopic Hebrew; do
  for weight in Regular Bold; do
    curl -fL --retry 3 -o "NotoSans${family}-${weight}.ttf" \
      "$base/NotoSans${family}/hinted/ttf/NotoSans${family}-${weight}.ttf"
  done
done
curl -fL --retry 3 -o OFL-NotoSans.txt \
  https://raw.githubusercontent.com/google/fonts/main/ofl/notosansdevanagari/OFL.txt
```

- [ ] **Step 2: Verify every download is a readable font with the expected coverage**

```bash
cd /tmp/noto-script-fonts
for f in *.ttf; do
  test -s "$f" || { echo "empty: $f"; exit 1; }
  fc-scan --format '%{family}\n' "$f" >/dev/null || { echo "bad font: $f"; exit 1; }
done
grep -m1 'SIL Open Font License' OFL-NotoSans.txt
```

Expected: no `bad font`/`empty` lines; the SIL OFL 1.1 line printed (the first line is the Noto copyright).

- [ ] **Step 3: Install into both model overlays**

```bash
set -euo pipefail
r1=/tmp/opencode/sonix-epub-scripts/sonix-packer/assets/R1/usr/resource/sonix/fonts
r3=/tmp/opencode/sonix-epub-scripts/sonix-packer/assets/R3PII/usr/resource/sonix/fonts
src=/tmp/noto-script-fonts
for family in devanagari tamil malayalam gurmukhi ethiopic hebrew; do
  fam="$(tr '[:lower:]' '[:upper:]' <<< "${family:0:1}")${family:1}"
  cp "$src/NotoSans${fam}-Regular.ttf" "$r1/$family.ttf"
  cp "$src/NotoSans${fam}-Bold.ttf"    "$r1/$family-bold.ttf"
done
cp "$src/OFL-NotoSans.txt" "$r1/OFL-NotoSans.txt"
cp "$r1"/{devanagari,tamil,malayalam,gurmukhi,ethiopic,hebrew}{,-bold}.ttf "$r3/"
cp "$r1/OFL-NotoSans.txt" "$r3/OFL-NotoSans.txt"
```

Expected: 13 new files in each directory.

- [ ] **Step 4: Confirm each weight covers its own chapters**

For every family, check the regular file and the bold file separately against the EPUB. The temporary tree holds Bookerly plus exactly one candidate face:

Each run still reports the other scripts as missing (and exits `1`), so the check asserts the owned chapters rather than the whole run:

```bash
set -euo pipefail
cd /tmp/opencode/sonix-epub-scripts
fonts=sonix-packer/assets/R1/usr/resource/sonix/fonts
epub="/home/diogo/Área de trabalho/Around the World in 28 Languages.epub"
check_one() { # $1 = chapter-label regex, $@ = font files to test together
  label=$1; shift
  rm -rf /tmp/fontcheck; mkdir -p /tmp/fontcheck
  cp "$fonts/ebook/Bookerly-Regular.ttf" /tmp/fontcheck/
  cp "$@" /tmp/fontcheck/
  out=$(python3 sonix-player/tools/epub_font_coverage.py --epub "$epub" --fonts /tmp/fontcheck || true)
  grep -Eq "^($label): ok$" <<<"$out" || { echo "FAIL: $label not covered by $*"; exit 1; }
  echo "ok: $label covered by $*"
}
check_one 'Nepali|Hindi|Marathi' "$fonts/devanagari.ttf"
check_one 'Nepali|Hindi|Marathi' "$fonts/devanagari-bold.ttf"
check_one 'Tamil'                "$fonts/tamil.ttf"
check_one 'Tamil'                "$fonts/tamil-bold.ttf"
check_one 'Malayalam'            "$fonts/malayalam.ttf"
check_one 'Malayalam'            "$fonts/malayalam-bold.ttf"
check_one 'Panjabi, Eastern'     "$fonts/gurmukhi.ttf"
check_one 'Panjabi, Eastern'     "$fonts/gurmukhi-bold.ttf"
check_one 'Amharic'              "$fonts/ethiopic.ttf"
check_one 'Amharic'              "$fonts/ethiopic-bold.ttf"
check_one 'Hebrew'               "$fonts/hebrew.ttf"
check_one 'Hebrew'               "$fonts/hebrew-bold.ttf"
check_one 'Chinese, Mandarin \(Simplified\)' "$fonts/default.otf"
check_one 'Chinese, Mandarin \(Simplified\)' "$fonts/bold.otf"
```

Expected: fourteen `ok: ... covered by ...` lines. This proves both weights carry the glyphs, not just their union.

- [ ] **Step 5: Run the full checker and confirm it now passes**

```bash
cd /tmp/opencode/sonix-epub-scripts
python3 sonix-player/tools/epub_font_coverage.py \
  --epub "/home/diogo/Área de trabalho/Around the World in 28 Languages.epub" \
  --fonts sonix-packer/assets/R1/usr/resource/sonix/fonts
```

Expected: every chapter `ok`, then `all chapters covered`, exit `0`.

- [ ] **Step 6: Commit**

```bash
git add sonix-packer/assets/R1/usr/resource/sonix/fonts sonix-packer/assets/R3PII/usr/resource/sonix/fonts
git commit -m "ebook: add Noto script faces for missing EPUB languages"
```

---

### Task 3: Wire the faces into the ebook fallback chain

**Files:**
- Modify: `sonix-player/src/gui/ebook/ebookfonts.c` (`SCRIPT_FACES[]`, near the top of the file)
- Test: compile the modified unit; full target build

**Interfaces:**
- Consumes: the font filenames from Task 2.
- Produces: `ebookfonts_open()` chains all eleven script faces (regular and bold) behind the primary ebook face. No public signature changes.

- [ ] **Step 1: Prepare the worktree build dependencies**

The ignored toolchain and third-party sources live in the main checkout; link them into the worktree:

```bash
set -euo pipefail
main=/home/diogo/Downloads/Hiby/sonix-player/sonix-player
wt=/tmp/opencode/sonix-epub-scripts/sonix-player
cd "$wt"
for d in rockbox-toolchain freetype-2.13.3 libogg-1.3.5 opus-1.4 opusfile-0.12 wavpack-5.6.0; do
  [ -e "$d" ] || ln -s "$main/$d" "$d"
done
test -x rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc
```

Expected: no output; cross compiler is executable.

- [ ] **Step 2: Extend `SCRIPT_FACES[]`**

Replace the current four-entry array with:

```c
static const script_face_t SCRIPT_FACES[] = {
	{"korean", "korean.otf", "korean-bold.otf"},
	{"chinese", "default.otf", "bold.otf"},
	{"japanese", "japanese.otf", "japanese-bold.otf"},
	{"thai", "thai.otf", "thai-bold.otf"},
	{"arabic", "arabic.otf", "arabic-bold.otf"},
	{"devanagari", "devanagari.ttf", "devanagari-bold.ttf"},
	{"tamil", "tamil.ttf", "tamil-bold.ttf"},
	{"malayalam", "malayalam.ttf", "malayalam-bold.ttf"},
	{"gurmukhi", "gurmukhi.ttf", "gurmukhi-bold.ttf"},
	{"ethiopic", "ethiopic.ttf", "ethiopic-bold.ttf"},
	{"hebrew", "hebrew.ttf", "hebrew-bold.ttf"},
};
```

Update the comment above it: Korean stays first for #190; Chinese is ahead of Japanese so Han uses MiSans.

- [ ] **Step 3: Compile the modified unit for the host**

```bash
cd /tmp/opencode/sonix-epub-scripts/sonix-player
make build_host/gui/ebook/ebookfonts.o
```

Expected: exit `0`, `build_host/gui/ebook/ebookfonts.o` created.

- [ ] **Step 4: Build the target and run the ABI check**

```bash
cd /tmp/opencode/sonix-epub-scripts/sonix-player
make target -j"$(nproc)"
```

Expected: `sonix_player` and `sonix_launch` built; `check-abi` prints `no symbol beyond glibc 2.22`.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/gui/ebook/ebookfonts.c
git commit -m "ebook: chain Noto script fallbacks for EPUBs"
```

---

### Task 4: Package and verify the R1 image

**Files:**
- No tracked files; produces `sonix-packer/r1.upt` (ignored build artifact)

**Interfaces:**
- Consumes: `sonix_player`, `sonix_launch`, the assets from Task 2, and a stock `r1_original.upt`.

- [ ] **Step 1: Assemble the packer inputs**

```bash
set -euo pipefail
cd /tmp/opencode/sonix-epub-scripts
cp sonix-player/sonix_player sonix-player/sonix_launch sonix-packer/
[ -e sonix-packer/r1_original.upt ] || \
  ln -s /home/diogo/Downloads/Hiby/sonix-player/sonix-packer/r1_original.upt sonix-packer/r1_original.upt
test -s sonix-packer/r1_original.upt
```

- [ ] **Step 2: Build the image**

```bash
cd /tmp/opencode/sonix-epub-scripts/sonix-packer
PATH="/home/diogo/Downloads/Hiby/fakebin:$PATH" ./sonix_firmware_packer.sh
```

Expected: `r1.upt` written; the R3 Pro II model is skipped or untouched because no `r3proii_original.upt` is present.

- [ ] **Step 2b: Record the image size**

```bash
stat -c '%n %s bytes' sonix-packer/r1.upt
```

Expected on this branch: `51,189,760` bytes (48.8 MiB), rootfs `47,824,896` bytes. The known-good 1.3.0 release is `47,996,928`. If the device rejects the image as too large, the first lever is the unhinted Noto files (about 0.5 MB smaller) or dropping the new scripts' bold faces.

- [ ] **Step 3: Confirm the twelve fonts are inside the packed rootfs**

```bash
cd /tmp/opencode/sonix-epub-scripts/sonix-packer
for f in devanagari tamil malayalam gurmukhi ethiopic hebrew; do
  for w in "" "-bold"; do
    test -s "squashfs-root/usr/resource/sonix/fonts/$f$w.ttf"
  done
done
test -s squashfs-root/usr/resource/sonix/fonts/OFL-NotoSans.txt
unsquashfs -l rootfs_original.squashfs 2>/dev/null | grep -E 'sonix/fonts/(devanagari|tamil|malayalam|gurmukhi|ethiopic|hebrew)' | wc -l
```

Expected: the loop succeeds; `unsquashfs` reports 12 (or the packer's rootfs listing shows all twelve).

- [ ] **Step 4: Confirm a missing script file is skipped, not fatal**

The loader must keep the `access(path, R_OK)` guard and the `if (!reg && !bld) continue;` skip in `fallbacks_open`. Verify both are still present:

```bash
grep -n 'access(path, R_OK)' sonix-player/src/gui/ebook/ebookfonts.c
grep -n 'if (!reg && !bld)' sonix-player/src/gui/ebook/ebookfonts.c
```

Expected: one match each. Then remove the Ethiopic files from the packed rootfs copy and confirm the tree still lists every other font; the design leaves the device run to the user:

```bash
mv sonix-packer/squashfs-root/usr/resource/sonix/fonts/ethiopic.ttf{,.off}
mv sonix-packer/squashfs-root/usr/resource/sonix/fonts/ethiopic-bold.ttf{,.off}
test -s sonix-packer/squashfs-root/usr/resource/sonix/fonts/hebrew.ttf
```

Expected: the Hebrew file still exists; restore the Ethiopic names afterwards.

- [ ] **Step 5: Install and verify on the HiBy R1**

Copy `r1.upt` to the root of the microSD, then on the device: `Settings > System > Update firmware > From SD card` (battery above 30%). Open the EPUB and check chapters 5, 8–13, 17 and 18 at the smallest and largest reader sizes. Also re-check one Latin chapter and the Korean, Japanese, Thai and Arabic chapters.

Expected: no `▯` in any chapter; Chinese and Amharic read normally; Hebrew and the Indic chapters show correct glyphs but may read in visual order / without conjuncts (documented limitation). Latin, Korean, Japanese, Thai and Arabic are unchanged.
