#!/usr/bin/env python3
"""Summarise a visit-distribution dump written by creatica's VisitDumpFile option.

The point of this file is not to convert data -- it is to answer, BEFORE any training is run,
whether the data contains anything worth learning.

Policy distillation can only teach the policy head what the SEARCH knows that the PRIOR does
not. If the search's preferred move is nearly always the prior's preferred move, the target is
almost the model's own output and training on it is a very expensive no-op. That has already
happened once in this project: top-k agreement with Stockfish rose from 26.7% to 34.8% while
Elo went +108, +113, level, because the extra agreement was on positions where several moves
were comparable anyway.

So the number to look at first is `search disagrees with prior`. If it is small, stop.

Usage:  python3 visit_dump_stats.py visits.tsv [more.tsv ...]
"""
import sys, math, collections


def read(path):
    with open(path) as f:
        for n, line in enumerate(f):
            line = line.rstrip("\n")
            if not line or line.startswith("tag\t"):
                continue
            parts = line.split("\t")
            if len(parts) != 8:
                yield None, n + 1, "expected 8 fields, got %d" % len(parts)
                continue
            tag, fen, sims, rootq, rootcp, ponder, seldep, visits = parts
            moves = []
            bad = None
            for tok in visits.split():
                bits = tok.split(":")
                if len(bits) != 3:
                    bad = "malformed move token %r" % tok
                    break
                try:
                    moves.append((bits[0], int(bits[1]), float(bits[2])))
                except ValueError:
                    bad = "unparseable move token %r" % tok
                    break
            if bad:
                yield None, n + 1, bad
                continue
            yield {
                "tag": tag, "fen": fen, "sims": int(sims), "rootq": float(rootq),
                "rootcp": int(rootcp), "ponder": ponder == "1", "seldepth": int(seldep), "moves": moves,
            }, n + 1, None


def entropy(ps):
    return -sum(p * math.log(p) for p in ps if p > 0)


def main(paths):
    recs, errors = [], []
    for p in paths:
        for rec, ln, err in read(p):
            (errors if err else recs).append((p, ln, err) if err else rec)

    if errors:
        print("  %d malformed line(s); first few:" % len(errors))
        for p, ln, err in errors[:3]:
            print("    %s:%d  %s" % (p, ln, err))
    if not recs:
        print("  no usable records"); return 1

    n = len(recs)
    ponder = sum(1 for r in recs if r["ponder"])
    tags = len({r["tag"] for r in recs if r["tag"] != "-"})
    dup = n - len({r["fen"] for r in recs})

    agree = 0                      # search's top move == prior's top move
    dupes = []                     # positions where a move appeared more than once
    top_share, ent, legal, sims = [], [], [], []
    shift = []                     # total-variation distance between prior and visit distribution
    for r in recs:
        mv = r["moves"]
        if not mv:
            continue
        total = sum(v for _, v, _ in mv)
        if total <= 0:
            continue
        best_visits = max(mv, key=lambda m: m[1])[0]
        best_prior = max(mv, key=lambda m: m[2])[0]
        agree += (best_visits == best_prior)
        # Lists, not dicts keyed by move. A dict silently merges duplicate move tokens, which
        # would hide an engine bug (the same move appearing twice in one root) behind a
        # plausible-looking distribution -- and it distorts every statistic below.
        names = [m[0] for m in mv]
        if len(set(names)) != len(names):
            dupes.append(r["fen"])
        psum = sum(m[2] for m in mv) or 1.0
        vshare = [m[1] / total for m in mv]
        pshare = [m[2] / psum for m in mv]
        shift.append(0.5 * sum(abs(v - p) for v, p in zip(vshare, pshare)))
        top_share.append(max(vshare))
        ent.append(entropy(vshare))
        legal.append(len(mv))
        sims.append(total)

    def avg(xs):
        return sum(xs) / len(xs) if xs else 0.0

    print("  records                    %d   (%d from ponder searches)" % (n, ponder))
    print("  distinct games (tags)      %s" % (tags if tags else "untagged"))
    print("  repeated positions         %d" % dup)
    if dupes:
        print("  DUPLICATE MOVES            %d position(s) list a move twice -- engine bug" % len(dupes))
    print("  legal moves, mean          %.1f" % avg(legal))
    print("  simulations, mean          %.0f" % avg(sims))
    print("  seldepth, mean             %.1f" % avg([r["seldepth"] for r in recs]))
    print("  top move's visit share     %.3f" % avg(top_share))
    print("  distribution entropy       %.3f nats  (uniform over %.1f = %.3f)"
          % (avg(ent), avg(legal), math.log(avg(legal)) if legal else 0))
    print()
    print("  --- is there anything to learn here? ---")
    if top_share:
        print("  search agrees with prior   %.1f%%  of positions on the top move" % (100.0 * agree / len(top_share)))
        print("  mean prior->visits shift   %.3f  (total-variation distance, 0 = identical)" % avg(shift))
        print()
        if agree / len(top_share) > 0.92 and avg(shift) < 0.10:
            print("  VERDICT: the search rarely departs from the prior. Distillation would mostly")
            print("           teach the model its own output. Not worth training on.")
        else:
            print("  VERDICT: the search moves substantially away from the prior, so the target")
            print("           carries information the policy head does not already have.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1:]))
