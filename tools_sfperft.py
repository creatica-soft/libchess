#!/usr/bin/env python3
"""Reference Chess960 perft counts from Stockfish, in ./perft suite format.

The suite file deliberately carries no 960 lines, with a note that any must be
generated against Stockfish under UCI_Chess960 rather than recalled -- a wrong
expectation would be indistinguishable from a generator bug. This produces them.
"""
import subprocess, sys, time

SF = "/Users/ap/stockfish-macos-m1-apple-silicon"

def main():
    fen_file = sys.argv[1]
    depths   = [int(d) for d in sys.argv[2].split(",")]
    limit    = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9

    fens = [l.strip() for l in open(fen_file) if l.strip() and not l.startswith("#")][:limit]

    p = subprocess.Popen([SF], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    def send(s):
        p.stdin.write(s + "\n"); p.stdin.flush()

    send("uci")
    while True:
        if p.stdout.readline().startswith("uciok"): break
    send("setoption name UCI_Chess960 value true")
    send("setoption name Threads value 1")
    send("isready")
    while True:
        if p.stdout.readline().startswith("readyok"): break

    t0 = time.time()
    for i, fen in enumerate(fens):
        counts = []
        for d in depths:
            send(f"position fen {fen}")
            send(f"go perft {d}")
            n = None
            while True:
                line = p.stdout.readline()
                if not line:
                    print(f"# stockfish died on {fen}", file=sys.stderr); sys.exit(1)
                if line.startswith("Nodes searched:"):
                    n = int(line.split(":")[1].strip()); break
            counts.append(f";D{d} {n}")
        print(f"{fen} {' '.join(counts)}", flush=True)
        if (i + 1) % 100 == 0:
            print(f"# {i+1}/{len(fens)}  {time.time()-t0:.0f}s", file=sys.stderr)
    send("quit")

main()
