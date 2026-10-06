# scan_games.py <games.pgn> <out.tsv> <movetime ms>: Stockfish score of every position in every game where the
# side to move is a creatica account, and of the position after its move. 8+ pieces only (below that the
# engine plays from tablebases). One row per creatica move:
#   game  ply  side  fen_before  move_uci  move_san  cp_before  cp_after  sf_best   (cp from the MOVER's point of view)
import re, subprocess, sys
pgn, out_path, ms = sys.argv[1], sys.argv[2], int(sys.argv[3])
SAN2FEN = _p("SAN2FEN", "~/libchess/mistakes/san2fen")
START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
games = []
for chunk in re.split(r"\n\n(?=\[Event )", open(pgn).read().strip()):
    tags = dict(re.findall(r'^\[(\w+) "(.*)"\]', chunk, re.M))
    body = "\n".join(l for l in chunk.splitlines() if not l.startswith("["))
    body = re.sub(r"\{[^}]*\}", " ", body)
    sans = [t for t in body.split() if not re.match(r"^(\d+\.+|1/2-1/2|1-0|0-1|\*)$", t)]
    games.append((tags, sans))
sf = subprocess.Popen([_p("STOCKFISH", "~/stockfish-macos-m1-apple-silicon")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
def send(s): sf.stdin.write(s + "\n"); sf.stdin.flush()
def until(prefix):
    last = None
    while True:
        l = sf.stdout.readline()
        if l.startswith("info") and " score " in l and " pv " in l: last = l
        if l.startswith(prefix): return l, last
send("uci"); until("uciok")
for k, v in (("Threads", 4), ("Hash", 512), ("SyzygyPath", _p("SYZYGY_PATH", "~/syzygy"))): send("setoption name %s value %s" % (k, v))
send("isready"); until("readyok")
cache = {}
def score(fen):   # cp from the side to move's point of view, and SF's best move
    if fen in cache: return cache[fen]
    send("position fen " + fen); send("go movetime %d" % ms)
    bm, info = until("bestmove")
    t = info.split() if info else []
    cp = 0
    if "score" in t:
        kind, val = t[t.index("score") + 1], int(t[t.index("score") + 2])
        cp = val if kind == "cp" else (10000 - abs(val)) * (1 if val > 0 else -1)
    cache[fen] = (cp, bm.split()[1] if len(bm.split()) > 1 else "")
    return cache[fen]
import os
import os
#Paths are overridable by environment variable and otherwise resolved under the home directory at run
#time: this is a public repository, so one machine's absolute paths do not belong in it.
def _p(var, default):
    return os.environ.get(var) or os.path.expanduser(default)
done = set()
if os.path.exists(out_path):
    done = {l.split("\t")[0] for l in open(out_path)}
    out = open(out_path, "a")
else:
    out = open(out_path, "w")
    out.write("game\tply\tside\tfen_before\tmove_uci\tmove_san\tcp_before\tcp_after\tsf_best\n")
for gi, (tags, sans) in enumerate(games):
    if not sans or tags.get("GameId") in done:
        continue
    start = tags.get("FEN", START)
    r = subprocess.run([SAN2FEN, start], input="\n".join(sans), capture_output=True, text=True, env={"EVERY_PLY": "1"})
    rows = [l.split("\t") for l in r.stdout.strip().splitlines()]
    if len(rows) < len(sans):
        print("skip", tags.get("GameId"), "replay failed", r.stderr.strip()[:80], flush=True); continue
    rows = rows[:len(sans)]
    fens = [start] + [x[1] for x in rows]
    ucis = rows[-1][2].split()
    for ply, san in enumerate(sans):
        before = fens[ply]; side = before.split()[1]
        mover = tags.get("White" if side == "w" else "Black", "").lower()
        if not mover.startswith("creaticachessbot"): continue
        if sum(c.isalpha() for c in before.split()[0]) <= 7: continue
        cb, best = score(before)
        ca, _ = score(fens[ply + 1])
        out.write("%s\t%d\t%s\t%s\t%s\t%s\t%d\t%d\t%s\n" % (tags.get("GameId"), ply, side, before, ucis[ply], san, cb, -ca, best))
    out.flush()
    print("game %d/%d %s done" % (gi + 1, len(games), tags.get("GameId")), flush=True)
send("quit")
