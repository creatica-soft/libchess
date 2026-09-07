#!/usr/bin/env python3
"""Turn eco.pgn into an opening book of FENs, for bot-vs-bot games on lichess.

    python3 make_book.py [--min-plies 6] [--max-plies 16] [--out book.tsv]

Two bots with the same engine and settings play very similar games from the start position, so
without a book a night of self-play revisits the same handful of lines. That wastes the games
twice over: as an Elo measurement it is a tiny effective sample, and as training data it is
mostly duplicate positions, each of which costs a full search to produce.

Depth band matters. Below ~6 plies the engines are still choosing their own opening and the book
changes nothing; past ~16 the line is long enough that the game is half over and the interesting
middlegame decisions were made by the book rather than by the engines. 6-16 leaves a real game
while guaranteeing the openings differ.

The FENs are emitted as-is. Whether they are BALANCED is a separate question this script does not
answer -- some ECO lines are dubious and start one side clearly worse. Filter with an engine:

    cut -f1 book.tsv | CREATICA_ENGINE=<stockfish> MOVETIME=1000 ./gen_targets > book_eval.tsv
    awk -F'\\t' 'sqrt($3*$3) < 100' book_eval.tsv > book_balanced.tsv

An unbalanced start is not useless -- it is still a distinct position, and for TRAINING data
variety is what matters. But for measuring Elo between two configurations it adds variance for
nothing, since both sides play both colours anyway.
"""
import json, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "gui_helper")
ECO = os.path.join(HERE, "eco.pgn")


def helper(commands):
    """One gui_helper process for all of it -- it is a line protocol, and spawning 1400
    processes to ask 1400 questions would take far longer than the parsing does."""
    p = subprocess.run([HELPER], input="\n".join(commands) + "\n",
                       capture_output=True, text=True)
    return [l for l in p.stdout.splitlines() if l.startswith("{")]


def main(argv):
    lo = int(argv[argv.index("--min-plies") + 1]) if "--min-plies" in argv else 6
    hi = int(argv[argv.index("--max-plies") + 1]) if "--max-plies" in argv else 16
    out = argv[argv.index("--out") + 1] if "--out" in argv else "book.tsv"

    listing = helper(["pgnlist " + ECO])
    if not listing:
        print("  could not read %s via %s" % (ECO, HELPER)); return 1
    games = json.loads(listing[0])["games"]
    want = [g for g in games if lo <= g["plies"] <= hi]
    print("  %d games, %d in the %d-%d ply band" % (len(games), len(want), lo, hi))

    lines = helper(["pgngame %d %s" % (g["index"], ECO) for g in want])
    rows, skipped = [], 0
    for raw in lines:
        try:
            g = json.loads(raw)
        except json.JSONDecodeError:
            skipped += 1; continue
        if not g.get("ok") or not g.get("moves"):
            skipped += 1; continue
        fen = g["moves"][-1]["fen"]
        t = g.get("tags", {})
        name = " ".join(x for x in (t.get("Opening", ""), t.get("Variation", "")) if x)
        rows.append((fen, t.get("ECO", "?"), name or "?", len(g["moves"])))

    # Distinct FENs: many ECO lines transpose into each other, and a duplicate start would
    # silently halve the variety the book is there to provide.
    seen, uniq = set(), []
    for r in rows:
        if r[0] not in seen:
            seen.add(r[0]); uniq.append(r)

    with open(out, "w") as fh:
        for fen, eco, name, plies in uniq:
            fh.write("%s\t%s\t%s\t%d\n" % (fen, eco, name, plies))
    print("  %d parsed, %d skipped, %d distinct positions -> %s"
          % (len(rows), skipped, len(uniq), out))
    print("  (%d transpositions collapsed)" % (len(rows) - len(uniq)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
