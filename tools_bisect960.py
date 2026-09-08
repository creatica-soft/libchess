#!/usr/bin/env python3
"""Walk a perft disagreement down to the single position and move that causes it.

Compares ./perft --divide against Stockfish 'go perft N' (UCI_Chess960 on), finds
the root move whose subtree count differs, plays it, and repeats one ply deeper.
At depth 1 the two move LISTS are diffed, which names the illegal move libchess
generates (or the legal one it misses).
"""
import subprocess, sys

SF = "/Users/ap/stockfish-macos-m1-apple-silicon"
PERFT = "/Users/ap/libchess/perft"

class Stock:
    def __init__(self):
        self.p = subprocess.Popen([SF], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        self.send("uci"); self.until("uciok")
        self.send("setoption name UCI_Chess960 value true")
        self.send("setoption name Threads value 1")
        self.send("isready"); self.until("readyok")
    def send(self, s): self.p.stdin.write(s + "\n"); self.p.stdin.flush()
    def until(self, pref):
        while True:
            l = self.p.stdout.readline()
            if not l: raise RuntimeError("stockfish died")
            if l.startswith(pref): return l
    def divide(self, fen, depth):
        self.send(f"position fen {fen}"); self.send(f"go perft {depth}")
        d = {}
        while True:
            l = self.p.stdout.readline()
            if not l: raise RuntimeError("stockfish died")
            if l.startswith("Nodes searched:"): return d
            if ":" in l:
                mv, _, n = l.partition(":")
                mv = mv.strip()
                if len(mv) in (4, 5) and mv[0].isalpha():
                    d[mv] = int(n.strip())
    def after(self, fen, mv):
        self.send(f"position fen {fen} moves {mv}"); self.send("d")
        return self.until("Fen:").split("Fen:")[1].strip()

def lib_divide(fen, depth):
    out = subprocess.run([PERFT, "--divide", "-d", str(depth), fen],
                         capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        if ":" in line and not line.startswith("---"):
            mv, _, n = line.partition(":")
            mv = mv.strip()
            if len(mv) in (4, 5) and mv[0].isalpha():
                try: d[mv] = int(n.strip())
                except ValueError: pass
    return d

def main():
    fen   = sys.argv[1]
    depth = int(sys.argv[2])
    sf = Stock()
    path = []
    while True:
        want = sf.divide(fen, depth)
        got  = lib_divide(fen, depth)
        if want == got:
            print(f"\nno disagreement at depth {depth} in {fen}"); return
        if depth == 1:
            extra   = sorted(set(got) - set(want))
            missing = sorted(set(want) - set(got))
            print("\n=== PINPOINTED ===")
            print("position :", fen)
            print("path     :", " ".join(path) if path else "(root)")
            if extra:   print("libchess generates ILLEGAL move(s):", " ".join(extra))
            if missing: print("libchess MISSES legal move(s)    :", " ".join(missing))
            print(f"stockfish {len(want)} moves, libchess {len(got)} moves")
            return
        # find a move whose subtree differs
        for mv in sorted(set(want) | set(got)):
            if want.get(mv) != got.get(mv):
                if mv not in want:
                    print("\n=== PINPOINTED ==="); print("position :", fen)
                    print("path     :", " ".join(path) if path else "(root)")
                    print("libchess generates ILLEGAL move:", mv); return
                if mv not in got:
                    print("\n=== PINPOINTED ==="); print("position :", fen)
                    print("path     :", " ".join(path) if path else "(root)")
                    print("libchess MISSES legal move:", mv); return
                print(f"  d{depth} {mv}: libchess {got[mv]} vs stockfish {want[mv]}  -> descending")
                path.append(mv)
                fen = sf.after(fen, mv)
                depth -= 1
                break

main()
