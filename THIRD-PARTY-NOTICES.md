# Third-party components and licence notices

This file inventories every piece of third-party code in the repository, says
where it came from and under what terms, and explains what that means for
anything you build here.

**The short version.** The top-level libchess sources are MIT. The `nnue/`
directory is a modified fork of Stockfish and is GPL-3. Because every build
target links the two together, **the library and the executables you get out of
this repository are GPL-3 works and must be redistributed under the GPL-3**, with
source. That is not a conflict — MIT is compatible with the GPL, and MIT code may
be combined into a GPL work — but it does mean "libchess is MIT" is only true of
the top-level source files, never of `libchess.dylib`, `creatica`, `lichess_bot`,
`tournament` or any of the `test_*` binaries.

---

## Stockfish — `nnue/` — GPL-3

- Copyright (C) 2004-2026 The Stockfish developers.
- Upstream: <https://github.com/official-stockfish/Stockfish>
- Contributors: the copyright headers say "see AUTHORS file"; that list is at
  <https://github.com/official-stockfish/Stockfish/blob/master/AUTHORS>
- Licence: GNU General Public License version 3 or later. Full text in `COPYING`.
- **Modified.** This is a heavily stripped fork of Stockfish 18, ported to
  evaluate libchess's own `Board`. The required modification notice, with dates
  and a summary of what changed, is in `nnue/README.md`.

This is the component that determines the licence of everything built here.
`add_library(chess SHARED ...)` in `CMakeLists.txt` compiles `nnue/evaluate.cpp`,
`nnue/bitboard.cpp`, `nnue/nnue/network.cpp` and their siblings directly into
`libchess.dylib`, and every executable links that library.

### The NNUE network files

`nn-c288c895ea92.nnue` and `nn-37f18f62d772.nnue` are Stockfish's trained
networks. Stockfish distributes its networks as part of the GPL-3 work, so the
same terms apply to them and to any binary they are embedded in. They are
**embedded into `libchess.dylib` at compile time** by the `incbin` assembler
directive in `nnue/nnue/network.cpp`, which is why the built library is about
110 MB.

The nets are deliberately not tracked in git because of their size. The filenames
are `#define`s in `nnue/evaluate.h` (`EvalFileDefaultNameBig` and
`EvalFileDefaultNameSmall`); download the matching files from
<https://tests.stockfishchess.org/nns> and place them in the repository root
before building.

## incbin — `nnue/incbin/incbin.h` — public domain

- Copyright: Dale Weiler.
- Upstream: <https://github.com/graphitemaster/incbin>
- Licence: released into the public domain under the Unlicense. Full text in
  `nnue/incbin/UNLICENCE`.
- Unmodified.

Used by `nnue/nnue/network.cpp` to embed the network files listed above.

## Syzygy tablebase probing — `tbcore.c`, `tbcore.h` — unrestricted

- Copyright (c) 2011-2015 Ronald de Man.
- Licence, quoted from the file: "This file may be redistributed and/or modified
  without restrictions."
- Upstream: Ronald de Man's Syzygy tablebase probing code.

Note that `tbcore.c` is not compiled on its own. `tbprobe.c` `#include`s it, so
it is deliberately absent from the target source lists in `CMakeLists.txt`.

## Fathom / Syzygy probing API — `tbprobe.c`, `tbprobe.h`, `tbconfig.h` — MIT

- Copyright (c) 2013-2015 Ronald de Man; (C) 2015 basil.
- Licence: MIT ("Permission is hereby granted, free of charge, ..."). The full
  notice is at the top of each of the three files.
- Upstream: <https://github.com/jdart1/Fathom>

MIT requires the copyright notice and permission notice to be included in all
copies or substantial portions of the software. They are retained verbatim in
the file headers, which covers source distribution; if you ship a **binary** of
`creatica`, `lichess_bot` or any `test_*` target to someone else, include this
notices file with it.

## nlohmann/json — `json.hpp` — MIT

- SPDX-FileCopyrightText: 2013 - 2025 Niels Lohmann <https://nlohmann.me>
- SPDX-License-Identifier: MIT
- Upstream: <https://github.com/nlohmann/json>
- Unmodified single-header release.

Live code: included by `creatica_search.hpp`, so it is part of the `creatica`
engine, and by `lichess_bot.cpp`. The same binary-distribution note as for Fathom
applies.

## uthash — `uthash.h` — BSD (revised)

- Copyright (c) 2003-2025, Troy D. Hanson <https://troydhanson.github.io/uthash/>
- Licence: revised BSD; the full notice is at the top of the file.
- Upstream: <https://github.com/troydhanson/uthash>

**Not used by any live target.** It is included only by files under `attic/` and
by unbuilt experiment sources in the repository root. It is listed here for
completeness; if it is ever removed, this entry can go with it.

## libcurl — linked, not vendored

`creatica` and `lichess_bot` link against the system libcurl for online
evaluation and tablebase lookups. No curl source is included in this repository,
so there is nothing to redistribute; curl's own licence is a permissive
MIT/X-derivative. See <https://curl.se/docs/copyright.html>.

---

## If you redistribute

1. Ship the whole thing under GPL-3, and include `COPYING`.
2. Include this file, so the MIT and BSD notices above travel with the code.
3. If you distribute binaries, GPL-3 section 6 requires you to also offer the
   corresponding source. Publishing the source repository alongside the binaries
   is the simplest way to do that.
4. `nnue/README.md` must stay with `nnue/`; it is the modification notice the
   GPL requires.

Nothing in this file is legal advice. It is an inventory, written so that the
obligations are visible rather than implicit.
