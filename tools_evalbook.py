#!/usr/bin/env python3
"""Evaluate every opening-book position with Stockfish and report the balance distribution.

make_book.py emits ECO lines as-is and says so: some of them start one side clearly
worse. The colour-swapped pairing cancels the BIAS, but it cannot make a decided
position interesting -- a +300cp opening just produces two foregone games instead
of one, which is the opposite of what the book is for.

Writes book.tsv with an extra cp column, from WHITE's point of view.
"""
import subprocess, sys

SF = "/Users/ap/stockfish-macos-m1-apple-silicon"

class Eng:
    def __init__(self, depth):
        self.depth = depth
        self.p = subprocess.Popen([SF], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        self.send("uci"); self.until("uciok")
        self.send("setoption name Threads value 1")
        self.send("setoption name Hash value 256")
        self.send("isready"); self.until("readyok")
    def send(self, s): self.p.stdin.write(s + "\n"); self.p.stdin.flush()
    def until(self, pref):
        while True:
            l = self.p.stdout.readline()
            if not l: raise RuntimeError("stockfish died")
            if l.startswith(pref): return l
    def cp(self, fen):
        self.send(f"position fen {fen}"); self.send(f"go depth {self.depth}")
        v = None
        while True:
            l = self.p.stdout.readline()
            if not l: raise RuntimeError("stockfish died")
            if l.startswith("bestmove"): break
            if " score cp " in l:
                try: v = int(l.split(" score cp ")[1].split()[0])
                except Exception: pass
            elif " score mate " in l:
                try:
                    m = int(l.split(" score mate ")[1].split()[0]); v = 10000 if m > 0 else -10000
                except Exception: pass
        if v is None: return None
        # Stockfish reports from the side to move; normalise to White.
        return v if fen.split()[1] == "w" else -v

def main():
    src   = sys.argv[1]
    depth = int(sys.argv[2]) if len(sys.argv) > 2 else 18
    out   = sys.argv[3] if len(sys.argv) > 3 else "/tmp/book_eval.tsv"
    rows = [l.rstrip("\n").split("\t") for l in open(src) if l.strip() and not l.startswith("#")]
    e = Eng(depth)
    scored = []
    with open(out, "w") as fh:
        for i, r in enumerate(rows):
            cp = e.cp(r[0])
            if cp is None: continue
            scored.append((cp, r))
            fh.write("\t".join(r) + f"\t{cp}\n"); fh.flush()
            if (i + 1) % 200 == 0: print(f"# {i+1}/{len(rows)}", file=sys.stderr, flush=True)
    e.send("quit")
    n = len(scored)
    a = sorted(abs(c) for c, _ in scored)
    print(f"\n  {n} positions, Stockfish depth {depth}, cp from White's point of view")
    print(f"  |cp| median {a[n//2]}   p75 {a[3*n//4]}   p90 {a[9*n//10]}   max {a[-1]}")
    for t in (50, 75, 100, 150, 200, 300):
        k = sum(1 for x in a if x > t)
        print(f"    beyond +/-{t:3}cp : {k:4}  ({100*k/n:5.1f}%)")
    print("\n  most lopsided:")
    for cp, r in sorted(scored, key=lambda x: -abs(x[0]))[:8]:
        print(f"    {cp:+6}cp  {r[2] if len(r)>2 else ''}")

main()
