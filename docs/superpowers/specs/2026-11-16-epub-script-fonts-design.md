# Remaining EPUB script fonts — short design

**Issue:** [Jepl4r/sonix-player#190](https://github.com/Jepl4r/sonix-player/issues/190) — follow-up to the Korean/Hangul fix in PR #269, using *Around the World in 28 Languages* as the test book.

## Intent / why

PR #269 added a glyph fallback chain behind Bookerly (`korean`, `japanese`, `thai`, `arabic`). The test book still shows `▯` in seven chapters because the firmware carries no font for those scripts. The EPUB's own DejaVu fonts do not help: `epub_css.c` skips `@font-face`, and DejaVu itself has no Indic, Ethiopic or Hebrew coverage.

This change adds the missing script faces to the ebook fallback chain. It is deliberately a glyph-coverage fix; full shaping/bidi is a separate project.

## What is missing today

Measured over every character in the 28 chapters against Bookerly plus the PR #269 chain:

| Chapter | Language | Script | Missing characters |
|---|---|---|---|
| 5 | Chinese (Simplified) | Han, simplified-only forms | 128 ideographs absent from `japanese.otf` |
| 8 | Tamil | Tamil | 42 |
| 9–11 | Nepali / Hindi / Marathi | Devanagari | 64–72 |
| 12 | Malayalam | Malayalam | 57 |
| 13 | Punjabi (Eastern) | Gurmukhi | 58 |
| 17 | Amharic | Ethiopic | 164 |
| 18 | Hebrew | Hebrew | 27 |

All other chapters already render (Maori, Tagalog, Japanese, Korean, Vietnamese, Thai, Russian, Arabic, Swahili, Polish, Greek, German, Italian, French, Spanish, Yoruba, English, Portuguese, Samoan).

## Approaches considered

1. **Font fallback only — chosen.** Add Noto Sans faces for the six missing scripts and reuse the MiSans files already in the firmware for Simplified Chinese. No layout changes. Solves every `▯`; complex scripts remain unshaped/unordered.
2. **Fallback + LVGL bidi and Arabic shaping.** Also corrects Hebrew direction and Arabic joining. More reader-layout work; Indic remains imperfect.
3. **Full HarfBuzz/FriBidi shaping.** Correct for every script, including Indic, but adds libraries, build changes and a layout rewrite. Out of scope here.

The user chose approach 1 for this delivery. Options 2 and 3 remain possible follow-ups.

Known limitation to state plainly in the PR: the reader draws one codepoint at a time (`place_letter` → `lv_draw_letter`) and has no shaping or bidi. After this change the glyphs are present, but:

- Hebrew and Arabic render left-to-right; Arabic letters are unjoined.
- Devanagari, Tamil, Malayalam and Gurmukhi render without conjuncts or vowel reordering.
- Chinese and Ethiopic are unaffected because they are left-to-right and need no shaping.

## Fonts to add

Static Noto Sans Regular/Bold from the Noto project (SIL OFL 1.1), copied into both model overlays at `usr/resource/sonix/fonts/`:

| File | Covers |
|---|---|
| `devanagari.ttf`, `devanagari-bold.ttf` | Hindi, Nepali, Marathi |
| `tamil.ttf`, `tamil-bold.ttf` | Tamil |
| `malayalam.ttf`, `malayalam-bold.ttf` | Malayalam |
| `gurmukhi.ttf`, `gurmukhi-bold.ttf` | Punjabi (Eastern) |
| `ethiopic.ttf`, `ethiopic-bold.ttf` | Amharic |
| `hebrew.ttf`, `hebrew-bold.ttf` | Hebrew |

Chinese reuses the existing `default.otf` (MiSans Regular) and `bold.otf` (MiSans Bold); no new Chinese font is shipped. MiSans has no Hangul, so it does not shadow `korean.otf`.

Total new flash: about 1.75 MB. `OFL-NotoSans.txt` accompanies the new files in each fonts folder.

Files go in `sonix-packer/assets/R1/usr/resource/sonix/fonts/` and `sonix-packer/assets/R3PII/usr/resource/sonix/fonts/`. The host simulator reads the same tree through `usr/resource/sonix/fonts`; `sonix-player/assets/fonts` is not used by the EPUB reader and is left alone.

## Code change

`sonix-player/src/gui/ebook/ebookfonts.c`: extend `SCRIPT_FACES[]` with seven entries, in this order:

```
korean      korean.otf         korean-bold.otf
chinese     default.otf        bold.otf
japanese    japanese.otf       japanese-bold.otf
thai        thai.otf           thai-bold.otf
arabic      arabic.otf         arabic-bold.otf
devanagari  devanagari.ttf     devanagari-bold.ttf
tamil       tamil.ttf          tamil-bold.ttf
malayalam   malayalam.ttf      malayalam-bold.ttf
gurmukhi    gurmukhi.ttf       gurmukhi-bold.ttf
ethiopic    ethiopic.ttf       ethiopic-bold.ttf
hebrew      hebrew.ttf         hebrew-bold.ttf
```

Korean stays first (the #190 guarantee). Chinese comes before Japanese so Han in a Chinese book uses MiSans forms instead of Rodin's Japanese forms; `korean.otf` has no Han, so it does not shadow MiSans. Japanese books are unaffected because `japanese.otf` is still their primary face (chosen by `ebook_is_japanese`). The existing `fallbacks_open`/`attach_fallbacks` lifecycle already handles a longer list and missing files; no header or reader change is needed.

Base branch: `fix/epub-korean-font-fallback` (PR #269), because the fallback machinery lives there. Rebase onto `main` when #269 merges.

## Constraints / risks

- **No shaping or bidi** — see limitation above. The PR must not claim correct Indic/Arabic typography.
- **Memory:** `fallbacks_open` opens every script face. FreeType maps the files and shares its glyph cache, so cost is face objects rather than decoded font data, but the R1 is a small device. If device testing shows pressure, a follow-up can open only the scripts found in the book's text.
- **Metrics:** Noto faces have different line metrics from Bookerly. Pagination may shift; the reader already copes with per-font metrics, but the chapters need visual checking.
- **Chinese style:** shared Han characters in a Chinese book now come from MiSans. Mixed Japanese books keep Rodin as primary.

## Validation

1. Coverage check: for every chapter of the attached EPUB, confirm each non-ASCII letter/punctuation codepoint exists in the new chain (Bookerly + the eleven script faces). Expected result: zero missing.
2. `make host` succeeds; open the EPUB in the simulator at several font sizes and page through chapters 5, 8–13, 17 and 18.
3. `make target` succeeds and `check-abi` reports no symbol newer than glibc 2.22.
4. Pack an R1-only image and confirm it contains the twelve new font files; install on the HiBy R1 and check the same chapters. Do not produce an `r3proii.upt`.
5. Regression: Latin chapters and the Korean/Japanese/Thai/Arabic chapters keep rendering as before.
