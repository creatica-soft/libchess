#!/usr/bin/env python3
"""Find positions where a deeper search overrules a shallower one -- creatica's own mistakes.

    python3 find_mistakes.py shallow.tsv deep.tsv [--fens out.txt] [--arbiter sf.tsv]

Both arguments are visit dumps (VisitDumpFile). The shallow one is typically what the bot wrote
during real games under a clock; the deep one is the same positions re-searched with a long
movetime. Positions present in both are compared.

WHY THIS SET IS WORTH MORE THAN ANY OTHER
-----------------------------------------
Training data has value only where the target differs from what the model already believes. A
position creatica handles well contributes almost no gradient -- the search confirms the prior
and there is nothing to learn.

Positions from human tournament games give breadth, but they do not contain creatica's OWN
errors: strong humans never reach the positions creatica reaches after its characteristic
mistakes, which is exactly where its blind spots are. This tool isolates those.

Disagreements come in two flavours and they are not equally interesting:

  ranked  the deep search's move was already in the shallow search's tree with real visits, but
          not on top. The policy was roughly right and the search just needed longer. Cheap to
          fix, less to learn.

  missed  the deep search's move got almost none of the shallow search's visits. The shallow
          search barely looked at the move that turns out to be best -- which is the failure
          mode that matters here, because with ~30 legal moves and a weak prior, a move that
          never gets visits can never be found however long you search.

A high `missed` share is the case for distillation: those are positions where more search time
would NOT have helped, and only a better prior can.

ARBITRATION (--arbiter) IS NOT OPTIONAL
--------------------------------------
A deeper search is a better reference, not a correct one -- it can talk itself into a worse move,
and where both searches share an NNUE blind spot it will be confidently wrong twice.

This is not hypothetical. On the first pair examined, of two disagreements:
  * one was a genuine policy failure (an underpromotion the shallow search gave 0.27% of visits)
  * one was the DEEP search being wrong, with the shallow search's move correct

Training on the second would have taught creatica a worse move. So run the mistake set past an
independent engine -- a different search algorithm with differently weighted evaluation, whose
blind spots are not yours:

    ./gen_targets < mistakes.txt          # with CREATICA_ENGINE=<stockfish>, no VISIT_DUMP
      -> "fen<TAB>bestmove<TAB>cp"        # feed that back here as --arbiter

Note this is NOT using Stockfish as a training target. Agreement with its PV1 across all
positions is the metric that saturated: top-1 went 26.7% -> 33.08% -> 34.83% while Elo went
+108 -> +113 -> level. Here it answers a narrower question on a small hard subset -- is this
specific move genuinely good? -- which is a use its saturation says nothing about.
"""
import sys


def load(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("tag\t"):
                continue
            p = line.split("\t")
            if len(p) != 8:
                continue
            mv = []
            for tok in p[7].split():
                b = tok.split(":")
                if len(b) == 3:
                    try:
                        mv.append((b[0], int(b[1])))
                    except ValueError:
                        pass
            if not mv:
                continue
            tot = sum(v for _, v in mv) or 1
            out[p[1]] = {
                "seldepth": int(p[6]),
                "top": max(mv, key=lambda m: m[1])[0],
                "share": {m: v / tot for m, v in mv},
                "line": line,
            }
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    shallow, deep = load(argv[1]), load(argv[2])
    out_fens = argv[argv.index("--fens") + 1] if "--fens" in argv else None
    arbiter = {}
    if "--arbiter" in argv:
        with open(argv[argv.index("--arbiter") + 1]) as fh:
            for line in fh:
                p = line.rstrip("\n").split("\t")
                if len(p) >= 2:
                    arbiter[p[0]] = p[1]

    common = [f for f in deep if f in shallow]
    if not common:
        print("  no positions in common -- is the deep run over the same FENs?")
        return 1

    agree, missed, ranked = 0, [], []
    for fen in common:
        s, d = shallow[fen], deep[fen]
        if s["top"] == d["top"]:
            agree += 1
            continue
        # how much attention did the shallow search give the move the deep search prefers?
        got = s["share"].get(d["top"], 0.0)
        (missed if got < 0.02 else ranked).append((fen, s["top"], d["top"], got,
                                                   s["seldepth"], d["seldepth"]))

    n = len(common)
    print(f"  positions compared        {n}")
    print(f"  mean seldepth  shallow    {sum(shallow[f]['seldepth'] for f in common)/n:.1f}")
    print(f"                 deep       {sum(deep[f]['seldepth'] for f in common)/n:.1f}")
    print(f"  deep agrees with shallow  {100*agree/n:.1f}%")
    print()
    print(f"  MISTAKES  {n-agree} ({100*(n-agree)/n:.1f}%)")
    print(f"    missed  {len(missed):5}  ({100*len(missed)/n:.1f}%)  shallow gave the better move <2% of visits")
    print(f"    ranked  {len(ranked):5}  ({100*len(ranked)/n:.1f}%)  it was in the tree, just not on top")
    if missed:
        print("\n  worst misses (shallow -> deep, visits the better move got):")
        for fen, st, dt, got, ss, ds in sorted(missed, key=lambda x: x[3])[:6]:
            print(f"    {st} -> {dt}  {100*got:5.2f}%   seldepth {ss}->{ds}   {fen[:44]}")

    keep = missed + ranked
    if arbiter:
        confirmed, rejected, unknown = [], [], []
        for row in keep:
            fen, st, dt = row[0], row[1], row[2]
            a = arbiter.get(fen)
            if a is None:      unknown.append(row)
            elif a == dt:      confirmed.append(row)
            else:              rejected.append(row)
        print()
        print(f"  ARBITRATED  ({len(arbiter)} verdicts available)")
        print(f"    confirmed {len(confirmed):5}  arbiter backs the deep search -- real policy failures")
        print(f"    REJECTED  {len(rejected):5}  arbiter backs the SHALLOW move -- the deep search was wrong")
        print(f"    unknown   {len(unknown):5}  no verdict for this position")
        if rejected:
            print("\n    rejected (training on these would teach a worse move):")
            for fen, st, dt, got, ss, ds in rejected[:5]:
                print(f"      deep said {dt}, arbiter says {arbiter[fen]} (shallow had {st})")
        keep = confirmed

    if out_fens:
        with open(out_fens, "w") as fh:
            for fen, *_ in keep:
                fh.write(fen + "\n")
        print(f"\n  wrote {len(keep)} FENs to {out_fens}")
        print("  -- " + ("arbiter-confirmed policy failures" if arbiter
                          else "UNARBITRATED: run an independent engine over these first"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
