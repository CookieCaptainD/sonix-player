# Integration base

Base commit: `7d540b91464c6610d08378ab464bc3fa9dfa177e`
Branch: `integration/r1-pr250-259`
Fork point: `fe94fe796d93c6efa50d1107a734864c9c6a03d5` (local `main`)

The base branch has 27 commits over `origin/main`. It carries PRs 250, 251,
252, 254, 255, 256, 258 and 259, plus the GitHub Actions build workflow from
the `github-actions-build` branch. Both new languages are present in the R1 and
R3PII language folders: `Português PT.ini` and `Ukrainian.ini`.

`src/system/core/lang.c` ends with the folded Latin sort from PR 259, including
the `latin_folds` table, `folded_letter()` and the byte-order tie break. PR 255's
simpler sort is superseded by PR 259.

The remaining 25 PRs (268 to 301) are merged in batches 2 to 10 of the plan.
