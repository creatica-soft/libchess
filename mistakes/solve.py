# solve.py <confirmed.tsv> <out.tsv> <engine> <movetime ms>: search each mistake position from an EMPTY tree with the
# bot's settings, and record which move the engine prefers after every second of search, and the root priors.
# SOLVED AT t: the preferred move is Stockfish-acceptable from t to the end of the search.
import subprocess, sys, os, threading, time, re, queue
conf, out_path, engine, ms = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
T = os.path.dirname(os.path.abspath(__file__))
dump = T + "/solve_dump.tsv"
if os.path.exists(dump): os.remove(dump)
rows = [l.rstrip("\n").split("\t") for l in open(conf)][1:]
p = subprocess.Popen([engine], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
                     cwd="/Users/ap/libchess", env=dict(os.environ, CREATICA_LOG=T + "/solve.log"))
q = queue.Queue()
threading.Thread(target=lambda: [q.put(l) for l in p.stdout], daemon=True).start()
def send(s): p.stdin.write(s + "\n"); p.stdin.flush()
opts = [("Hash", 2048), ("Threads", 4), ("SyzygyPath", "/Users/ap/syzygy"), ("PolicyWeights", "/Users/ap/libchess/nnue_policy_pisp2.bin"),
        ("BlendScale", 102), ("TreeResetBelow", 1000), ("TreeResetHollow", 900), ("MultiPV", 1),
        ("IntermittentInfoLines", "true"), ("FinalInfoLines", "true"), ("VisitDumpFile", dump)]
send("uci")
for k, v in opts: send("setoption name %s value %s" % (k, v))
out = open(out_path, "w")
out.write("game\tply\tplayed\tacceptable\tsolved_at_ms\tfinal_move\tfinal_sims\tsamples\n")
for i, (g, ply, fen, mv, san, bcp, pcp, acc, lines) in enumerate(rows):
    accset = set(acc.replace("+more", "").split())
    send("ucinewgame"); send("isready")
    while not q.get().startswith("readyok"): pass
    send("position fen " + fen); send("go movetime %d" % ms)
    samples = []; t0 = time.time()
    while True:
        l = q.get()
        if l.startswith("bestmove"): final = l.split()[1]; break
        if l.startswith("info") and " multipv 1 " in l and " pv " in l:
            t = l.split()
            samples.append((int(t[t.index("nodes") + 1]), int(float(t[t.index("time") + 1])), t[t.index("pv") + 1]))
    # one sample per printed second; the final info line repeats the last state
    solved = None
    for k in range(len(samples)):
        if all(s[2] in accset for s in samples[k:]): solved = samples[k][1]; break
    if final not in accset: solved = None
    sims = samples[-1][0] if samples else 0
    out.write("%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\n" % (g, ply, mv, acc, solved if solved is not None else "-", final, sims,
                                                   " ".join("%d:%s" % (s[1], s[2]) for s in samples)))
    out.flush()
    print("%d/%d %s ply %s: played %s, engine now %s, solved at %s ms" % (i + 1, len(rows), g, ply, mv, final, solved), flush=True)
send("quit")
