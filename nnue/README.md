# nnue/ — a modified fork of Stockfish

**This directory contains modified versions of files from Stockfish, and those
modifications are not part of upstream Stockfish.** Stockfish is Copyright (C)
2004-2026 The Stockfish developers and is licensed under the GNU General Public
License version 3. The full licence text is in `COPYING` at the root of this
repository. Upstream is <https://github.com/official-stockfish/Stockfish>, and
the contributor list the copyright headers refer to as "the AUTHORS file" is at
<https://github.com/official-stockfish/Stockfish/blob/master/AUTHORS>.

This notice exists to satisfy section 5(a) of the GPL, which requires a modified
work to carry prominent notices stating that it was changed and giving a date.

## What was changed, and when

The fork was taken from Stockfish 18 (`nnue/misc.cpp` still reports
`version = "18"`). It was first committed here on **2025-06-16** and has been
modified continuously since; the most recent change as of this notice is
**2026-09-07**. The individual commits are in this repository's git history.

The changes are substantial. In summary:

- **Everything that is not evaluation has been deleted.** Stockfish's search,
  transposition table, move generation, move picking, thread pool, UCI layer and
  `position.cpp` are all gone. What remains is the NNUE evaluation and the
  bitboard, memory and helper code it needs.

- **The evaluation was ported to a different board representation.** Upstream
  NNUE reads Stockfish's `Position` class. This fork reads libchess's own
  `struct Board`, which is defined in `chess_types.h` at the repository root.
  `nnue/board.h` is a one-line forwarder to that header, kept under this name so
  that the ten `#include "../board.h"` lines in the vendored tree stay
  byte-identical to upstream and remain easy to re-sync.

- **A wrapper was added.** `nnue/nnue.cpp` is new code written for this project,
  not a modified Stockfish file. It is the interface the rest of libchess uses:
  `init_nnue`, `init_nnue_context`, `evaluate_nnue`, the accumulator
  push/pop/reset calls and `nnue_eval`.

- **A policy head was added** on top of the network, which upstream does not
  have. Files ending in `.prepolicy` and `.withfeat` are retained snapshots from
  that work.

## Licence of this directory

Everything in this directory is distributed under the GPL version 3, including
the files written for this project. The MIT licence in `LICENSE` at the
repository root covers the top-level libchess sources, not this directory, and
anything built by linking the two together is a GPL-3 work. See
`THIRD-PARTY-NOTICES.md` for the full picture.

`nnue/incbin/` is a separate third-party component under its own terms; see
`nnue/incbin/UNLICENCE`.
