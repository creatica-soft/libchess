#!/usr/bin/env python3
"""Generate Chess960 MIDGAME positions in which castling is still legal.

The 960 start positions barely test castling: it is not reachable in the four
plies the count suite runs, and the undo_move() bug found there needed one
specific line. The interesting positions are the ones where the back rank has
opened up, the king and rooks are still home, and castling is legal RIGHT NOW --
including the cases where the rook's castling destination is its own square.

Stockfish does the walking, so the positions do not depend on libchess's own move
generation being correct.
"""
import subprocess, sys, random

SF = "/Users/ap/stockfish-macos-m1-apple-silicon"

class SF960:
    def __init__(self):
        self.p = subprocess.Popen([SF], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        self.send("uci"); self.until("uciok")
        self.send("setoption name UCI_Chess960 value true")
        self.send("setoption name Threads value 1")
        self.send("isready"); self.until("readyok")

    def send(self, s):
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def until(self, pref):
        while True:
            l = self.p.stdout.readline()
            if not l: raise RuntimeError("stockfish died")
            if l.startswith(pref): return l

    def legal_moves(self, root, moves):
        self.send(f"position fen {root}" + (" moves " + " ".join(moves) if moves else ""))
        self.send("go perft 1")
        out = []
        while True:
            l = self.p.stdout.readline()
            if not l: raise RuntimeError("stockfish died")
            if l.startswith("Nodes searched:"): break
            if ":" in l:
                mv = l.split(":")[0].strip()
                if len(mv) in (4, 5) and mv[0].isalpha(): out.append(mv)
        return out

    def fen(self, root, moves):
        self.send(f"position fen {root}" + (" moves " + " ".join(moves) if moves else ""))
        self.send("d")
        return self.until("Fen:").split("Fen:")[1].strip()


def castling_available(fen, moves):
    """True if some legal move is the king moving onto one of its OWN rooks."""
    board, stm = fen.split()[0], fen.split()[1]
    castle = fen.split()[2]
    if castle == "-": return False
    # map squares -> piece
    sq = {}
    for r, row in enumerate(board.split("/")):
        f = 0
        for c in row:
            if c.isdigit(): f += int(c)
            else:
                sq["abcdefgh"[f] + str(8 - r)] = c; f += 1
    king = "K" if stm == "w" else "k"
    rook = "R" if stm == "w" else "r"
    ksq = [s for s, p in sq.items() if p == king]
    if not ksq: return False
    ksq = ksq[0]
    for mv in moves:
        if mv[:2] == ksq and sq.get(mv[2:4]) == rook:
            return True
    return False


def main():
    starts   = [l.strip() for l in open(sys.argv[1]) if l.strip()]
    want     = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    max_ply  = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    rng = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 20260908)
    sf = SF960()

    seen, kept = set(), 0
    tries = 0
    while kept < want and tries < want * 30:
        tries += 1
        root = rng.choice(starts)
        moves = []
        for _ in range(rng.randint(6, max_ply)):
            legal = sf.legal_moves(root, moves)
            if not legal: break
            # Prefer not to move the king, so castling rights survive the walk.
            moves.append(rng.choice(legal))
        legal = sf.legal_moves(root, moves)
        if not legal: continue
        fen = sf.fen(root, moves)
        if not castling_available(fen, legal): continue
        if fen in seen: continue
        seen.add(fen); kept += 1
        print(fen, flush=True)
        if kept % 50 == 0: print(f"# {kept}/{want} (tries {tries})", file=sys.stderr)
    sf.send("quit")

main()
