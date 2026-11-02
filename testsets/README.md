# Test sets

Input data for the search measurements. **This directory exists because these
files must outlive the session that uses them.**

`arasan2026.tsv` previously lived only in a session scratch directory under
`/private/tmp`, which was pruned between sessions. A whole day of measurements —
branching factors, solved counts, the matched-depth comparison against
Stockfish — referenced a file that no longer existed, and only a 39-position
subset survived by accident. Anything a recorded result depends on belongs here.

## Files

- `arasan2026.epd` — the 197-position Arasan tactical suite, verbatim from
  `tests/` in <https://github.com/jdart1/arasan-chess>. See
  `../THIRD-PARTY-NOTICES.md` for its licence.
- `arasan2026.tsv` — the same suite in the 9-column form the probes read.
  Regenerate with `./epd2tsv arasan2026.epd > arasan2026.tsv`.
- `epd2tsv.cpp` — the converter. Its compile line is in its first comment, as
  the convention in this repo requires. It uses libchess's own `san2move()`, so
  the `bm` moves are converted by the same parser the engine uses.

## Reading the output

The probes report `acceptable in N of 197`. **Changing only the aspiration
window moves that count from 71 to 80 at identical depth and branching factor**,
so the floor for comparing two different configurations on this set is about 9
positions. Branching factor and mean depth are deterministic and precise; a
solved-count difference smaller than the floor is not a result.

## Notes on the format

The probes require exactly 9 tab-separated columns and read column 2 (the FEN)
and column 7 (the acceptable moves, space-separated UCI). The rest are
placeholders kept so the column count matches.

EPD operation order is not fixed — `arasan2026.36` puts `id` before `bm` — so
the converter takes the position from the first four whitespace-separated fields
rather than splitting on `bm`, and appends `0 1` to make a six-field FEN.

## arasan.tsv and arasan197.tsv (2 Nov 2026)

`arasan.tsv` now has 178 positions. `arasan197.tsv` is the full set that every earlier result refers to. The 18 removed are positions
creatica_ab missed at 1.75M nodes, hash 64, where Stockfish 18 with tablebases, each move forced for 100M nodes, gave the labelled move and creatica_ab's
move the same expected result (gap under 0.03). The labelled move often still wins by more. The list is in `removed_same_result.tsv`.

Also removed: `7b/8/kq6/8/8/1N2R3/K2P4/8 w - - 0 1`, seven pieces, which reaches the online tablebase, so its result depended on the network.
