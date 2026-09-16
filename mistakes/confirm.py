# confirm.py <scan.tsv> <out.tsv> <ms>: pick creatica moves that lost >= 100 cp in a game that was not already
# decided, and re-score each position with Stockfish MultiPV 8 at <ms>. A move is ACCEPTABLE if Stockfish scores
# it within 50 cp of its best. Positions where the deeper search no longer shows a >= 100 cp loss are dropped.
import subprocess, sys
scan, out_path, ms = sys.argv[1], sys.argv[2], int(sys.argv[3])
rows = [l.rstrip("\n").split("\t") for l in open(scan)][1:]
cands = []
for g, ply, side, fen, mv, san, cb, ca, best in rows:
    cb, ca = int(cb), int(ca)
    if cb - ca < 100: continue
    if ca >= 400 or cb <= -400: continue          # still clearly winning after it, or already clearly lost
    cands.append((g, int(ply), fen, mv, san, cb, ca))
print(len(cands), "candidates from", len(rows), "moves", flush=True)
sf = subprocess.Popen(["/Users/ap/stockfish-macos-m1-apple-silicon"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
def send(s): sf.stdin.write(s + "\n"); sf.stdin.flush()
def run(fen, extra, multipv):
    send("setoption name MultiPV value %d" % multipv); send("position fen " + fen); send("go movetime %d %s" % (ms, extra))
    lines = {}
    while True:
        l = sf.stdout.readline()
        if l.startswith("bestmove"): break
        t = l.split()
        if t[:1] == ["info"] and "multipv" in t and "score" in t and "pv" in t:
            kind, val = t[t.index("score") + 1], int(t[t.index("score") + 2])
            cp = val if kind == "cp" else (10000 - abs(val)) * (1 if val > 0 else -1)
            lines[int(t[t.index("multipv") + 1])] = (t[t.index("pv") + 1], cp)
    return [lines[k] for k in sorted(lines)]
send("uci")
for k, v in (("Threads", 4), ("Hash", 1024), ("SyzygyPath", "/Users/ap/syzygy")): send("setoption name %s value %s" % (k, v))
send("isready")
while not sf.stdout.readline().startswith("readyok"): pass
out = open(out_path, "w")
out.write("game\tply\tfen\tplayed_uci\tplayed_san\tbest_cp\tplayed_cp\tacceptable\tsf_lines\n")
kept = 0
for i, (g, ply, fen, mv, san, cb, ca) in enumerate(cands):
    send("ucinewgame")
    lines = run(fen, "", 8)
    if not lines: continue
    best_cp = lines[0][1]
    played = dict(lines).get(mv)
    if played is None:
        pl = run(fen, "searchmoves " + mv, 1)
        played = pl[0][1] if pl else best_cp
    if best_cp - played < 100: continue
    acc = [m for m, cp in lines if cp >= best_cp - 50]
    open_ended = len(lines) == 8 and lines[-1][1] >= best_cp - 50
    out.write("%s\t%d\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n" % (g, ply, fen, mv, san, best_cp, played, " ".join(acc) + (" +more" if open_ended else ""),
                                                   " ".join("%s:%d" % x for x in lines)))
    out.flush(); kept += 1
    print("%d/%d kept %d" % (i + 1, len(cands), kept), flush=True)
send("quit")
