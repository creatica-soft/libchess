#!/usr/bin/env python3
"""
chess_gui.py -- Component B of the libchess GUI.

A small local web server that serves chess_gui.html and a JSON API on top of:
  * ./gui_helper   (Component A) for move legality, SAN and terminal detection
  * ./creatica*    UCI engines, for analysis and engine-vs-engine tournaments

Standard library only.  Run with:

    python3 chess_gui.py [--port 8080]

Binds to 127.0.0.1 only.  This is a local tool and is deliberately not
reachable from the network.
"""

from __future__ import annotations

import argparse
import atexit
import collections
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
HELPER_PATH = os.path.join(SCRIPT_DIR, "gui_helper")
HTML_PATH = os.path.join(SCRIPT_DIR, "chess_gui.html")

START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

# Default opening book, the same ten positions tournament_nnue_policy.cpp uses.
#
# Every game starting from the initial array makes a match far less informative than it
# looks: the engines repeat the same struggle, draws pile up, and the result is dominated
# by whatever both engines happen to do in one opening. Each position is played TWICE with
# the colours swapped, so a book entry that simply favours White cannot bias the score.
DEFAULT_OPENINGS = [
    "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",   # 1.e4 e5
    "rnbqkbnr/pppp1ppp/8/4p3/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",   # 1.d4 e5
    "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",   # Sicilian
    "rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",   # Caro-Kann
    "rnbqkbnr/pp1ppppp/8/2p5/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",   # Old Benoni
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",       # start position
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",     # 1.e4
    "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq - 0 1",     # 1.d4
    "rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b KQkq - 0 1",     # 1.c4
    "rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1",     # 1.Nf3
]

# The engines each hold a ~110 MB embedded NNUE net, so keep only a few alive.
MAX_LIVE_ENGINES = 4

# Startup is slow (net load + tablebase init); be generous.
HANDSHAKE_TIMEOUT = 120.0

# "movetime plus 10 seconds", per the API contract.
SEARCH_TIMEOUT_SLACK = 10.0

# After a search overruns we ask the engine to "stop" and give it this long to
# produce its bestmove before we give up on the process entirely.
STOP_GRACE = 3.0

# gui_helper answers a single line per command; it should never take long.
HELPER_TIMEOUT = 20.0

# Safety net so a tournament game cannot run forever if adjudication is missed.
MAX_PLIES = 600

DEFAULT_MOVETIME_MS = 2000
DEFAULT_GAMES = 20


class ApiError(Exception):
    """A bad request or an unusable configuration -> ok:false, not a 500."""


class EngineError(Exception):
    """An engine process failed, died, or timed out."""


# --------------------------------------------------------------------------
# Reading subprocess output without ever blocking the server forever
# --------------------------------------------------------------------------

class LineReader:
    """Pumps a text pipe into a queue on a daemon thread.

    readline() distinguishes three outcomes:
        a string  -> a line (with its trailing newline)
        ""        -> timed out, the process is still alive
        None      -> EOF, the process closed its stdout
    """

    def __init__(self, stream):
        self.stream = stream
        self.q: queue.Queue = queue.Queue()
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self):
        try:
            for line in self.stream:
                self.q.put(line)
        except Exception:
            pass
        finally:
            self.q.put(None)

    def readline(self, timeout):
        try:
            return self.q.get(timeout=max(0.0, timeout))
        except queue.Empty:
            return ""

    def drain(self):
        """Discard anything buffered; used before starting a fresh search."""
        while True:
            try:
                if self.q.get_nowait() is None:
                    self.q.put(None)  # keep the EOF sentinel for the next read
                    return
            except queue.Empty:
                return


def _tail_reader(stream, sink):
    """Collect a process's stderr into a bounded deque for error messages."""
    def run():
        try:
            for line in stream:
                sink.append(line.rstrip("\n"))
        except Exception:
            pass
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


# --------------------------------------------------------------------------
# Component A: the long-lived gui_helper process
# --------------------------------------------------------------------------

class GuiHelper:
    """One long-lived ./gui_helper, spoken to line by line under a lock.

    Restarts automatically if it dies.  If the binary is missing entirely the
    API returns a clean ok:false instead of the server failing to start.
    """

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.proc = None
        self.reader = None
        self.stderr_tail = collections.deque(maxlen=20)

    # -- process lifecycle -------------------------------------------------

    def _alive(self):
        return self.proc is not None and self.proc.poll() is None

    def _start(self):
        if not os.path.isfile(self.path):
            raise ApiError(
                "gui_helper not found at %s -- build Component A first" % self.path)
        if not os.access(self.path, os.X_OK):
            raise ApiError("gui_helper at %s is not executable" % self.path)
        self.stderr_tail.clear()
        self.proc = subprocess.Popen(
            [self.path],
            cwd=SCRIPT_DIR,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.reader = LineReader(self.proc.stdout)
        _tail_reader(self.proc.stderr, self.stderr_tail)

    def _kill(self):
        proc, self.proc, self.reader = self.proc, None, None
        if proc is None:
            return
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.kill()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            pass

    def shutdown(self):
        # gui_helper is specified to run until stdin closes, so closing stdin
        # is the clean shutdown here (unlike the engines, which need "quit").
        with self.lock:
            proc, self.proc, self.reader = self.proc, None, None
            if proc is None or proc.poll() is not None:
                return
            try:
                proc.stdin.close()
            except Exception:
                pass
            try:
                proc.wait(timeout=5)
            except Exception:
                try:
                    proc.kill()
                    proc.wait(timeout=5)
                except Exception:
                    pass

    # -- the one operation ------------------------------------------------

    def command(self, line):
        """Send one command line, return the decoded JSON reply as a dict.

        Never raises: every failure is reported as {"ok": false, "error": ...}
        so the HTTP layer can hand it straight back to the UI.
        """
        with self.lock:
            for attempt in (0, 1):  # one automatic restart-and-retry
                try:
                    if not self._alive():
                        self._start()
                    self.proc.stdin.write(line + "\n")
                    self.proc.stdin.flush()
                except ApiError as exc:
                    return {"ok": False, "error": str(exc)}
                except Exception as exc:
                    self._kill()
                    if attempt == 0:
                        continue
                    return {"ok": False,
                            "error": "gui_helper write failed: %s" % exc}

                reply, err = self._read_reply()
                if reply is not None:
                    return reply
                self._kill()
                if attempt == 0:
                    continue
                return {"ok": False, "error": err}
        # unreachable
        return {"ok": False, "error": "gui_helper: unreachable"}

    def _read_reply(self):
        """Read one JSON line.  Returns (dict, None) or (None, error string)."""
        deadline = time.time() + HELPER_TIMEOUT
        blanks = 0
        while True:
            line = self.reader.readline(deadline - time.time())
            if line is None:
                tail = "; ".join(list(self.stderr_tail)[-3:])
                return None, ("gui_helper exited without answering"
                              + (" (stderr: %s)" % tail if tail else ""))
            if line == "":
                return None, ("gui_helper did not answer within %.0fs"
                              % HELPER_TIMEOUT)
            text = line.strip()
            if not text:
                # Assumption: a stray blank line is tolerated rather than
                # treated as a protocol violation.  A few in a row is a fault.
                blanks += 1
                if blanks > 10:
                    return None, "gui_helper produced only blank lines"
                continue
            try:
                obj = json.loads(text)
            except ValueError:
                return None, ("gui_helper returned non-JSON output: %r"
                              % text[:200])
            if not isinstance(obj, dict):
                return None, ("gui_helper returned JSON that is not an object: %r"
                              % text[:200])
            return obj, None


HELPER = GuiHelper(HELPER_PATH)


# --------------------------------------------------------------------------
# UCI engines
# --------------------------------------------------------------------------

def parse_info_line(line):
    """Pull depth / nodes / score / pv out of one UCI 'info' line."""
    out = {"depth": 0, "nodes": 0, "pv": [], "multipv": 1,
           "score_cp": None, "score_mate": None}
    toks = line.split()
    i = 0
    while i < len(toks):
        tok = toks[i]
        try:
            if tok == "depth" and i + 1 < len(toks):
                out["depth"] = int(toks[i + 1])
                i += 1
            elif tok == "nodes" and i + 1 < len(toks):
                out["nodes"] = int(toks[i + 1])
                i += 1
            elif tok == "multipv" and i + 1 < len(toks):
                out["multipv"] = int(toks[i + 1])
                i += 1
            elif tok == "score" and i + 2 < len(toks):
                kind, val = toks[i + 1], int(toks[i + 2])
                if kind == "cp":
                    out["score_cp"] = val
                elif kind == "mate":
                    out["score_mate"] = val
                i += 2
            elif tok == "pv":
                out["pv"] = toks[i + 1:]
                break
        except ValueError:
            pass  # malformed number: ignore this token, keep parsing
        i += 1
    return out


def pick_info(infos, bestmove):
    """Choose which 'info' line describes the move the engine actually played.

    The contract says "the LAST info line that carries score".  Both creatica
    binaries default to MultiPV 5, so the literal last scored line is usually
    the *fifth-best* move -- e.g. bestmove e2e4 while the final info line
    scores c2c3.  Reporting that score would be plainly wrong.

    So: prefer the last scored line whose pv starts with the bestmove, then the
    last scored "multipv 1" line, and only then fall back to the literal last
    scored line.  For a single-PV engine all three rules select the same line,
    so this matches the contract exactly where the contract is unambiguous.
    """
    scored = [x for x in infos
              if x["score_cp"] is not None or x["score_mate"] is not None]
    if not scored:
        return None
    if bestmove:
        matching = [x for x in scored if x["pv"] and x["pv"][0] == bestmove]
        if matching:
            return matching[-1]
    primary = [x for x in scored if x["multipv"] == 1]
    if primary:
        return primary[-1]
    return scored[-1]


def clean_options(raw):
    """Sanitise a {name: value} map of UCI options from the browser.

    Values reach the engine inside a setoption line, so a newline would let a caller inject
    a second command. Names and values are therefore stripped of control characters, and
    anything empty is dropped rather than sent as a malformed line.
    """
    if raw is None:
        return {}
    if not isinstance(raw, dict):
        raise ApiError("'options' must be an object of name -> value")
    out = {}
    for k, v in raw.items():
        name = "".join(c for c in str(k) if c.isprintable()).strip()
        val = "".join(c for c in str(v) if c.isprintable()).strip()
        if not name or val == "":
            continue
        out[name] = val
    return out


def parse_option_block(lines):
    """Parse "option name <N> type <T> [default D] [min A] [max B]" lines.

    The name may contain spaces, so this scans for the keywords rather than splitting on
    whitespace. An option with an empty name is skipped: the older engine emits one
    (`option name  type spin default 0 min 0 max 0`), and offering a nameless control would
    only produce a blank row that cannot be set.
    """
    out = []
    for raw in lines:
        if not raw.startswith("option name "):
            continue
        rest = raw[len("option name "):]
        ti = rest.find(" type ")
        if ti < 0:
            continue
        name = rest[:ti].strip()
        if not name:
            continue
        tail = rest[ti + 6:].split()
        if not tail:
            continue
        entry = {"name": name, "type": tail[0]}
        for key in ("default", "min", "max"):
            if key in tail:
                k = tail.index(key)
                if k + 1 < len(tail):
                    # a string default may contain spaces and runs to the end
                    if key == "default" and entry["type"] == "string":
                        entry[key] = " ".join(tail[k + 1:])
                    else:
                        entry[key] = tail[k + 1]
        out.append(entry)
    return out


def engine_key(name, env_overrides, options=None):
    """Identity of an engine configuration, for reuse across requests.

    Options are part of the identity, not just the environment. Two configurations of the
    same binary that differ only by a setoption are different engines for our purposes, and
    handing back a pooled process still carrying the previous configuration is the kind of
    bug that produces a match measuring nothing.
    """
    return (name,
            tuple(sorted(env_overrides.items())),
            tuple(sorted((options or {}).items())))


class UciEngine:
    """One UCI engine process with a fixed environment.

    self.lock serialises use of the process; the pool holds it for the duration
    of a search so two callers can never interleave commands.
    """

    def __init__(self, name, env_overrides, options=None):
        self.name = name
        self.env_overrides = dict(env_overrides)
        # name -> value, applied with setoption after the uci handshake
        self.options = dict(options or {})
        self.key = engine_key(name, self.env_overrides, self.options)
        self.lock = threading.RLock()
        self.last_used = time.time()
        self.proc = None
        self.reader = None
        self.stderr_tail = collections.deque(maxlen=20)

    def is_alive(self):
        return self.proc is not None and self.proc.poll() is None

    # -- lifecycle --------------------------------------------------------

    def start(self):
        path = os.path.join(SCRIPT_DIR, self.name)
        env = os.environ.copy()
        env.update(self.env_overrides)
        self.stderr_tail.clear()
        try:
            self.proc = subprocess.Popen(
                [path],
                cwd=SCRIPT_DIR,
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
            )
        except OSError as exc:
            self.proc = None
            raise EngineError("could not start %s: %s" % (self.name, exc))
        self.reader = LineReader(self.proc.stdout)
        _tail_reader(self.proc.stderr, self.stderr_tail)
        self._send("uci")
        advertised = self._wait_for("uciok", HANDSHAKE_TIMEOUT)
        self.advertised = parse_option_block(advertised)
        # setoption goes between uciok and the first isready, which is what the protocol
        # asks for and what every GUI does.
        for name, value in self.options.items():
            if not str(name).strip():
                continue
            self._send("setoption name %s value %s" % (name, value))
        self._send("isready")
        self._wait_for("readyok", HANDSHAKE_TIMEOUT)

    def _stderr_note(self):
        tail = "; ".join(list(self.stderr_tail)[-3:])
        return " (stderr: %s)" % tail if tail else ""

    def _send(self, text):
        if not self.is_alive():
            raise EngineError("%s is not running" % self.name)
        try:
            self.proc.stdin.write(text + "\n")
            self.proc.stdin.flush()
        except Exception as exc:
            raise EngineError("%s: write failed: %s%s"
                              % (self.name, exc, self._stderr_note()))

    def _wait_for(self, token, timeout):
        """Read until `token`; returns every line seen on the way.

        The lines are what carries the "option name ..." block after "uci", which is how
        the browser learns what an engine can be configured with instead of the UI holding
        a hand-maintained copy of the list.
        """
        deadline = time.time() + timeout
        seen = []
        while True:
            line = self.reader.readline(deadline - time.time())
            if line is None:
                raise EngineError("%s exited before '%s'%s"
                                  % (self.name, token, self._stderr_note()))
            if line == "":
                raise EngineError("%s did not answer '%s' within %.0fs%s"
                                  % (self.name, token, timeout,
                                     self._stderr_note()))
            if line.strip() == token:
                return seen
            seen.append(line.rstrip())

    def new_game(self):
        """Best effort reset between tournament games."""
        self._send("ucinewgame")
        self._send("isready")
        self._wait_for("readyok", HANDSHAKE_TIMEOUT)

    def hard_stop(self):
        """Kill without ceremony; used when the process is already suspect."""
        proc, self.proc, self.reader = self.proc, None, None
        if proc is None:
            return
        try:
            proc.kill()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            pass

    def quit(self):
        """Clean shutdown.

        Always send "quit": creatica-shared-root does NOT handle having its
        stdin closed underneath it and prints "libc++abi: terminating".
        """
        proc, self.proc, self.reader = self.proc, None, None
        if proc is None or proc.poll() is not None:
            return
        try:
            proc.stdin.write("quit\n")
            proc.stdin.flush()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
                proc.wait(timeout=5)
            except Exception:
                pass
        try:
            proc.stdin.close()
        except Exception:
            pass

    # -- searching --------------------------------------------------------

    def search(self, fen, movetime_ms):
        self.reader.drain()
        self._send("position fen " + fen)
        self._send("go movetime %d" % int(movetime_ms))

        deadline = time.time() + movetime_ms / 1000.0 + SEARCH_TIMEOUT_SLACK
        infos = []
        last_raw = ""
        asked_to_stop = False

        while True:
            line = self.reader.readline(deadline - time.time())

            if line is None:
                raise EngineError("%s exited during search%s"
                                  % (self.name, self._stderr_note()))

            if line == "":
                if asked_to_stop:
                    raise EngineError(
                        "%s did not return a bestmove after 'stop'"
                        % self.name)
                # Overran movetime + slack: ask it to stop, then allow a short
                # grace period for the bestmove it owes us.
                self._send("stop")
                asked_to_stop = True
                deadline = time.time() + STOP_GRACE
                continue

            text = line.strip()
            if not text:
                continue

            if text.startswith("info"):
                if " score " in " " + text + " ":
                    last_raw = text
                    infos.append(parse_info_line(text))
                continue

            if text.startswith("bestmove"):
                parts = text.split()
                best = parts[1] if len(parts) > 1 else None
                ponder = None
                if len(parts) > 3 and parts[2] == "ponder":
                    ponder = parts[3]
                # Some engines report "(none)" / "0000" with no legal move.
                if best in ("(none)", "0000", "none"):
                    best = None
                chosen = pick_info(infos, best)
                result = {
                    "ok": True,
                    "bestmove": best,
                    "ponder": ponder,
                    # depth/nodes default to 0 when the engine sent no scored
                    # info line at all; the contract always shows them present.
                    "depth": chosen["depth"] if chosen else 0,
                    "nodes": chosen["nodes"] if chosen else 0,
                    "pv": chosen["pv"] if chosen else [],
                    "info": last_raw,
                }
                if chosen and chosen["score_mate"] is not None:
                    result["score_mate"] = chosen["score_mate"]
                elif chosen and chosen["score_cp"] is not None:
                    result["score_cp"] = chosen["score_cp"]
                return result

            # Anything else (id / option / "info string" without score) is
            # protocol noise during a search; ignore it.


class EnginePool:
    """Keeps a handful of engines alive and hands them out under their lock."""

    def __init__(self):
        self.lock = threading.Lock()
        self.engines = {}

    def acquire(self, name, env_overrides, options=None):
        """Return a started engine with its lock HELD by the caller."""
        key = engine_key(name, env_overrides, options)
        victims = []
        with self.lock:
            eng = self.engines.get(key)
            if eng is None:
                eng = UciEngine(name, env_overrides, options)
                while len(self.engines) >= MAX_LIVE_ENGINES:
                    cand = None
                    for other in sorted(self.engines.values(),
                                        key=lambda e: e.last_used):
                        if other.lock.acquire(blocking=False):
                            cand = other
                            break
                    if cand is None:
                        break  # everything is busy; run over the soft cap
                    del self.engines[cand.key]
                    victims.append(cand)
                self.engines[key] = eng
            eng.last_used = time.time()

        # Shut the evicted engines down outside the pool lock.
        for victim in victims:
            try:
                victim.quit()
            finally:
                victim.lock.release()

        eng.lock.acquire()
        try:
            if not eng.is_alive():
                eng.start()
        except Exception:
            eng.lock.release()
            self.forget(eng)
            raise
        return eng

    def release(self, eng):
        eng.last_used = time.time()
        eng.lock.release()

    def forget(self, eng):
        """Drop a broken engine from the pool and kill it.  Leaves locks alone."""
        with self.lock:
            if self.engines.get(eng.key) is eng:
                del self.engines[eng.key]
        eng.hard_stop()

    def shutdown(self):
        """Quit every engine.  In parallel, so shutdown stays prompt."""
        with self.lock:
            engines = list(self.engines.values())
            self.engines.clear()

        def stop_one(eng):
            try:
                eng.quit()
            except Exception:
                try:
                    eng.hard_stop()
                except Exception:
                    pass

        threads = [threading.Thread(target=stop_one, args=(e,), daemon=True)
                   for e in engines]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=12.0)
        # Anything that ignored "quit" gets killed outright rather than
        # being left orphaned.
        for eng in engines:
            try:
                eng.hard_stop()
            except Exception:
                pass


POOL = EnginePool()


def resolve_engine_name(name):
    """Validate an engine name and return it.

    Only executables in this directory whose names begin with "creatica" may be
    launched; this keeps the API from being a way to run arbitrary programs.
    """
    if not isinstance(name, str) or not name.strip():
        raise ApiError("an engine name is required")
    name = name.strip()
    if os.path.basename(name) != name or name.startswith("."):
        raise ApiError("invalid engine name: %r" % name)
    if not name.startswith("creatica"):
        raise ApiError("engine names must start with 'creatica': %r" % name)
    path = os.path.join(SCRIPT_DIR, name)
    if not (os.path.isfile(path) and os.access(path, os.X_OK)):
        raise ApiError("no such engine: %s" % name)
    return name


def clean_env(raw):
    """Normalise an optional {"VAR":"value"} block from a request body."""
    if raw is None:
        return {}
    if not isinstance(raw, dict):
        raise ApiError("'env' must be an object of string values")
    out = {}
    for k, v in raw.items():
        if not isinstance(k, str) or not k:
            raise ApiError("'env' keys must be non-empty strings")
        if v is None:
            continue
        # Numbers in JSON are convenient for the UI; coerce them to strings.
        out[k] = v if isinstance(v, str) else str(v)
    return out


def clamp_movetime(value):
    try:
        ms = int(value)
    except (TypeError, ValueError):
        ms = DEFAULT_MOVETIME_MS
    return max(10, min(ms, 600000))


def run_search(name, env_overrides, fen, movetime_ms, options=None):
    """Acquire an engine, search, and always release it."""
    eng = POOL.acquire(name, env_overrides, options)
    try:
        return eng.search(fen, movetime_ms)
    except EngineError:
        POOL.forget(eng)
        raise
    finally:
        eng.lock.release()


def list_engines():
    """Executable files in this directory whose names start with 'creatica'."""
    names = []
    try:
        with os.scandir(SCRIPT_DIR) as entries:
            for entry in entries:
                if not entry.name.startswith("creatica"):
                    continue
                try:
                    if not entry.is_file(follow_symlinks=True):
                        continue
                except OSError:
                    continue
                if os.access(entry.path, os.X_OK):
                    names.append(entry.name)
    except OSError:
        return []
    return sorted(names)


# --------------------------------------------------------------------------
# Tournaments
# --------------------------------------------------------------------------

def fen_repetition_key(fen):
    """Only placement, side to move, castling and en passant.

    The half-move and full-move counters must be excluded or a repetition can
    never match.
    """
    return " ".join(fen.split()[:4])


def fen_halfmove_clock(fen):
    parts = fen.split()
    if len(parts) >= 5:
        try:
            return int(parts[4])
        except ValueError:
            return 0
    return 0


class Tournament:
    """One engine-vs-engine match, played on a background thread."""

    def __init__(self, tid, white_cfg, black_cfg, movetime_ms, games,
                 openings=None):
        self.id = tid
        # Paired: entry k is used by games 2k+1 and 2k+2, so both configurations get
        # each opening from both sides.
        self.openings = list(openings) if openings else list(DEFAULT_OPENINGS)
        self.white_cfg = white_cfg   # config that has White in game 1
        self.black_cfg = black_cfg
        self.movetime_ms = movetime_ms
        self.games = games
        self.stop_flag = threading.Event()
        self.lock = threading.Lock()
        self.thread = None
        # One PGN per match, named so concurrent or successive matches never
        # append into each other's file.
        self.pgn_name = "match_%s_%s.pgn" % (time.strftime("%Y%m%d_%H%M%S"), tid[:6])
        self.state = {
            "running": True,
            "game": 0,
            "games": games,
            "score": {"white_config": 0.0, "black_config": 0.0},
            "board": {
                "fen": START_FEN,
                "last_move": None,
                "white": white_cfg["engine"],
                "black": black_cfg["engine"],
            },
            "moves": [],
            "results": [],
            "start_fen": START_FEN,
            "pgn": self.pgn_name,
        }

    # -- shared state ------------------------------------------------------

    def snapshot(self):
        """Cheap, non-blocking copy for /api/tournament/state."""
        with self.lock:
            st = self.state
            return {
                "ok": True,
                "running": st["running"],
                "game": st["game"],
                "games": st["games"],
                "score": dict(st["score"]),
                "board": dict(st["board"]),
                "moves": list(st["moves"]),
                "pgn": st.get("pgn"),
                "openings": len(self.openings),
                "results": [dict(r) for r in st["results"]],
                **({"error": st["error"]} if st.get("error") else {}),
            }

    def start(self):
        self.thread = threading.Thread(target=self._run, daemon=True,
                                       name="tournament-%s" % self.id)
        self.thread.start()

    def stop(self):
        self.stop_flag.set()

    # -- the match ---------------------------------------------------------

    def _run(self):
        try:
            for game_no in range(1, self.games + 1):
                if self.stop_flag.is_set():
                    break
                # Game 1: the "white" config has White.  Game 2: it has Black.
                swapped = (game_no % 2 == 0)
                white = self.black_cfg if swapped else self.white_cfg
                black = self.white_cfg if swapped else self.black_cfg
                opening = self.openings[((game_no - 1) // 2) % len(self.openings)]

                with self.lock:
                    self.state["game"] = game_no
                    self.state["moves"] = []
                    self.state["start_fen"] = opening
                    self.state["board"] = {
                        "fen": opening,
                        "last_move": None,
                        "white": white["engine"],
                        "black": black["engine"],
                    }

                for cfg in (white, black):
                    self._new_game(cfg)

                result, reason, plies = self._play_game(white, black, opening)

                # A game abandoned by /api/tournament/stop was never finished,
                # so it is neither recorded nor scored: crediting half a point
                # to each side would corrupt the very comparison the match
                # exists to make.
                if result is None:
                    break

                w_pts, b_pts = {"1-0": (1.0, 0.0),
                                "0-1": (0.0, 1.0)}.get(result, (0.5, 0.5))
                with self.lock:
                    score = self.state["score"]
                    if swapped:
                        score["white_config"] += b_pts
                        score["black_config"] += w_pts
                    else:
                        score["white_config"] += w_pts
                        score["black_config"] += b_pts
                    self.state["results"].append({
                        "game": game_no,
                        "result": result,
                        "plies": plies,
                        "reason": reason,
                    })
                    sans = list(self.state["moves"])
                    wname = self.state["board"].get("white") or "white"
                    bname = self.state["board"].get("black") or "black"
                    start_fen = self.state.get("start_fen") or START_FEN
                # Written outside the lock: a finished game should never hold up
                # /api/tournament/state, which the UI polls twice a second.
                try:
                    self._append_pgn(game_no, wname, bname, result, reason,
                                     sans, start_fen, white, black)
                except Exception as exc:
                    # Never let a disk problem kill a match in progress.
                    sys.stderr.write("pgn save failed for game %d: %s\n" % (game_no, exc))
                if self.stop_flag.is_set():
                    break
        except Exception as exc:  # never let the thread die silently
            with self.lock:
                self.state["error"] = "tournament failed: %s" % exc
        finally:
            with self.lock:
                self.state["running"] = False

    def _new_game(self, cfg):
        try:
            eng = POOL.acquire(cfg["engine"], cfg["env"], cfg.get("options"))
        except Exception:
            return  # the failure will surface on the first search
        try:
            eng.new_game()
        except Exception:
            POOL.forget(eng)
        finally:
            eng.lock.release()

    @staticmethod
    def _describe(cfg):
        """One line describing how a side was configured, for the PGN."""
        opts = (cfg or {}).get("options") or {}
        env = (cfg or {}).get("env") or {}
        parts = ["%s=%s" % (k, opts[k]) for k in sorted(opts)]
        parts += ["%s=%s" % (k, env[k]) for k in sorted(env)]
        return " ".join(parts) if parts else "defaults"

    def _append_pgn(self, game_no, wname, bname, result, reason, sans, start_fen,
                    white_cfg=None, black_cfg=None):
        """Append one finished game to this match's PGN file.

        Games are saved as they finish rather than at the end of the match, so a
        match that is stopped early still leaves behind everything it played.
        """
        os.makedirs(GAMES_DIR, exist_ok=True)
        tags = {
            "Event": "creatica GUI match",
            "Site": "local",
            "Date": time.strftime("%Y.%m.%d"),
            "Round": str(game_no),
            "White": wname,
            "Black": bname,
            "Result": result,
        }
        if reason:
            tags["Termination"] = reason
        # Which binary and which settings each side actually ran.
        #
        # Without this a match PGN is unattributable after the fact: both sides are often
        # the same binary and the seven-tag roster records only the engine's id string, so
        # two files comparing entirely different configurations look identical. It also
        # catches the failure where settings never reached the engine -- if both sides read
        # "defaults" in a match that was supposed to compare two configurations, the match
        # measured nothing, and that is far easier to notice here than by counting threads
        # in a process listing.
        tags["WhiteEngine"] = (white_cfg or {}).get("engine", wname)
        tags["BlackEngine"] = (black_cfg or {}).get("engine", bname)
        tags["WhiteOptions"] = self._describe(white_cfg)
        tags["BlackOptions"] = self._describe(black_cfg)
        tags["TimeControl"] = "movetime/%dms" % self.movetime_ms
        text = format_pgn(tags, sans, start_fen)
        with open(os.path.join(GAMES_DIR, self.pgn_name), "a",
                  encoding="utf-8") as fh:
            fh.write(text + "\n")

    def _play_game(self, white, black, opening=None):
        """Play one game.

        Returns (result, reason, plies), or (None, None, plies) if the match
        was stopped part way through this game.
        """
        fen = opening or START_FEN
        plies = 0
        reps = {}

        while True:
            if self.stop_flag.is_set():
                return None, None, plies

            info = HELPER.command("legal " + fen)
            if not info.get("ok"):
                return "1/2-1/2", "helper error: %s" % info.get("error"), plies

            turn = info.get("turn", "w")
            status = info.get("status", "ok")

            if status == "mate":
                # The side to move is mated, so the other side won.
                return ("0-1" if turn == "w" else "1-0"), "mate", plies
            if status == "stalemate":
                return "1/2-1/2", "stalemate", plies

            legal = info.get("moves") or []
            if not legal:
                # Defensive: no moves but status said otherwise.
                return "1/2-1/2", "no legal moves", plies

            key = fen_repetition_key(fen)
            reps[key] = reps.get(key, 0) + 1
            if reps[key] >= 3:
                return "1/2-1/2", "threefold", plies
            if fen_halfmove_clock(fen) >= 100:
                return "1/2-1/2", "fifty-move", plies
            if plies >= MAX_PLIES:
                return "1/2-1/2", "move limit", plies

            cfg = white if turn == "w" else black
            loser_result = "0-1" if turn == "w" else "1-0"

            try:
                # cfg["options"] must be passed here, not only in _new_game().
                # The pool keys an engine by (binary, env, options), so omitting them
                # resolves every search to a DIFFERENT engine running defaults -- both
                # sides sharing one process, and the two configured engines receiving
                # nothing but ucinewgame. A match run that way compares nothing while
                # looking entirely normal.
                res = run_search(cfg["engine"], cfg["env"], fen,
                                 self.movetime_ms, cfg.get("options"))
            except (EngineError, ApiError) as exc:
                return loser_result, "engine error: %s" % exc, plies

            best = res.get("bestmove")
            if not best:
                return loser_result, "engine returned no move", plies
            if best not in legal:
                return loser_result, "illegal move %s" % best, plies

            moved = HELPER.command("move %s %s" % (best, fen))
            if not moved.get("ok"):
                return "1/2-1/2", "helper error: %s" % moved.get("error"), plies

            fen = moved.get("fen") or fen
            plies += 1
            with self.lock:
                self.state["moves"].append(moved.get("san") or best)
                self.state["board"]["fen"] = fen
                self.state["board"]["last_move"] = best


TOURNAMENTS = {}
TOURNAMENTS_LOCK = threading.Lock()


def get_tournament(tid):
    with TOURNAMENTS_LOCK:
        return TOURNAMENTS.get(tid)


def reap_tournaments():
    """Keep the finished-match table from growing without bound."""
    with TOURNAMENTS_LOCK:
        if len(TOURNAMENTS) <= 8:
            return
        finished = [(t.state.get("game", 0), tid)
                    for tid, t in TOURNAMENTS.items()
                    if not t.state.get("running")]
        for _, tid in finished[:len(TOURNAMENTS) - 8]:
            TOURNAMENTS.pop(tid, None)


# --------------------------------------------------------------------------
# API handlers
# --------------------------------------------------------------------------

def api_legal(body):
    fen = body.get("fen") or START_FEN
    if not isinstance(fen, str):
        raise ApiError("'fen' must be a string")
    # gui_helper owns validation; pass its answer straight through.
    return HELPER.command("legal " + fen.strip())


def api_move(body):
    fen = body.get("fen") or START_FEN
    move = body.get("move")
    if not isinstance(fen, str):
        raise ApiError("'fen' must be a string")
    if not isinstance(move, str) or not move.strip() or " " in move.strip():
        raise ApiError("'move' must be a single long-algebraic move")
    # The move comes first so the FEN, which contains spaces, is the rest.
    return HELPER.command("move %s %s" % (move.strip(), fen.strip()))


def api_engines():
    return {"ok": True, "engines": list_engines()}


def api_analyse(body):
    fen = body.get("fen") or START_FEN
    if not isinstance(fen, str):
        raise ApiError("'fen' must be a string")
    name = resolve_engine_name(body.get("engine"))
    env_overrides = clean_env(body.get("env"))
    options = clean_options(body.get("options"))
    movetime = clamp_movetime(body.get("movetime_ms", DEFAULT_MOVETIME_MS))
    try:
        return run_search(name, env_overrides, fen.strip(), movetime, options)
    except (EngineError, ApiError) as exc:
        return {"ok": False, "error": str(exc)}


def parse_side(raw, label):
    if not isinstance(raw, dict):
        raise ApiError("'%s' must be an object with an 'engine' field" % label)
    return {
        "engine": resolve_engine_name(raw.get("engine")),
        "env": clean_env(raw.get("env")),
        "options": clean_options(raw.get("options")),
    }


def _parse_openings(raw):
    """Accept a list of FENs, or one block of text with a FEN per line.

    Blank lines and anything after a '#' are ignored so a book can carry comments.
    Each entry is validated through gui_helper before the match starts: a bad FEN
    discovered on game 7 would waste everything played up to it.
    """
    if raw is None:
        return None
    if isinstance(raw, str):
        items = [ln.split("#", 1)[0].strip() for ln in raw.splitlines()]
    elif isinstance(raw, list):
        items = [str(x).split("#", 1)[0].strip() for x in raw]
    else:
        raise ApiError("'openings' must be a list of FENs or a block of text")
    items = [x for x in items if x]
    if not items:
        return None
    if len(items) > 200:
        raise ApiError("that is more than 200 opening positions")
    for fen in items:
        chk = HELPER.command("legal " + fen)
        if not chk.get("ok"):
            raise ApiError("opening position rejected: %s (%s)"
                           % (fen, chk.get("error", "unknown")))
        if not chk.get("moves"):
            raise ApiError("opening position has no legal moves: %s" % fen)
    return items


def api_tournament_start(body):
    white = parse_side(body.get("white"), "white")
    black = parse_side(body.get("black"), "black")
    movetime = clamp_movetime(body.get("movetime_ms", DEFAULT_MOVETIME_MS))
    try:
        games = int(body.get("games", DEFAULT_GAMES))
    except (TypeError, ValueError):
        games = DEFAULT_GAMES
    games = max(1, min(games, 1000))

    tid = uuid.uuid4().hex[:12]
    openings = _parse_openings(body.get("openings"))
    tour = Tournament(tid, white, black, movetime, games, openings)
    reap_tournaments()
    with TOURNAMENTS_LOCK:
        TOURNAMENTS[tid] = tour
    tour.start()
    return {"ok": True, "id": tid}


def api_tournament_state(tid):
    if not tid:
        raise ApiError("'id' is required")
    tour = get_tournament(tid)
    if tour is None:
        raise ApiError("no such tournament: %s" % tid)
    return tour.snapshot()


def api_tournament_stop(body):
    tid = body.get("id")
    if not isinstance(tid, str) or not tid:
        raise ApiError("'id' is required")
    tour = get_tournament(tid)
    if tour is None:
        raise ApiError("no such tournament: %s" % tid)
    tour.stop()
    return {"ok": True}


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

# -- saved games ----------------------------------------------------------
#
# PGN files live in a "games" directory beside this script so a game played or
# watched here can be re-opened in the Analyse tab later. Names are sanitised
# hard: a saved name can only ever produce a file directly inside that
# directory, because the name arrives from the browser and "../" in a filename
# is how a local tool becomes a way to overwrite arbitrary files.

GAMES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "games")


# Directories a PGN may be opened from. Two, both fixed: the games directory this tool
# writes to, and the project directory beside the script, where the match harnesses drop
# their PGNs. Everything else is refused.
#
# The guard is not there to stop the person running this -- they have a shell. It is
# there because the server listens on a port that any page in their browser can POST to,
# and with DNS rebinding such a page can become same-origin and read the replies. Without
# a containment check, "../../../.ssh/id_rsa" would be readable that way. Widening this
# to two known directories keeps that closed while making the files people actually have
# reachable.
PGN_ROOTS = [GAMES_DIR, SCRIPT_DIR]


def _safe_pgn_name(name):
    """Sanitise a name for WRITING: always a plain basename inside GAMES_DIR."""
    base = os.path.basename(str(name or "").strip())
    keep = [c for c in base if c.isalnum() or c in "-_. "]
    base = "".join(keep).strip().strip(".")
    if not base:
        raise ApiError("empty or unusable file name")
    if not base.lower().endswith(".pgn"):
        base += ".pgn"
    if len(base) > 120:
        base = base[-120:]
    return base


def _resolve_pgn(name):
    """Resolve a name for READING to a real path inside one of PGN_ROOTS.

    Accepts either a bare file name or one prefixed with the root it came from, as
    the listing returns them. Containment is checked on the REALPATH, after symlinks
    are resolved, so neither "../" nor a symlink planted in games/ can escape.
    """
    raw = str(name or "").strip()
    if not raw:
        raise ApiError("no file name given")
    base = os.path.basename(raw)
    for root in PGN_ROOTS:
        cand = os.path.realpath(os.path.join(root, base))
        rootr = os.path.realpath(root)
        if not (cand == rootr or cand.startswith(rootr + os.sep)):
            continue
        if os.path.isfile(cand):
            return cand
    raise ApiError("no such PGN in the games or project directory: %s" % base)


def api_pgn_save(body):
    pgn = body.get("pgn")
    if not isinstance(pgn, str) or not pgn.strip():
        raise ApiError("no pgn content")
    name = _safe_pgn_name(body.get("name") or "game.pgn")
    os.makedirs(GAMES_DIR, exist_ok=True)
    path = os.path.join(GAMES_DIR, name)
    # Append rather than truncate when the file exists, so a tournament can add
    # game after game to one file and a second save never silently destroys the
    # first.
    mode = "a" if (body.get("append") and os.path.exists(path)) else "w"
    with open(path, mode, encoding="utf-8") as fh:
        if mode == "a":
            fh.write("\n")
        # Always leave a trailing blank line. libchess's initGame() returns the final
        # game of a file WITHOUT stripping move numbers, comments or the result when it
        # hits EOF while reading moves; a blank line after the last game means EOF is
        # not reached there and the game parses normally. gui_helper repairs the raw
        # case anyway, but files written here should be well formed for any other tool.
        fh.write(pgn.rstrip() + "\n\n")
    return {"ok": True, "name": name, "path": path,
            "bytes": os.path.getsize(path)}


def format_pgn(tags, sans, start_fen):
    """Render one game as PGN text.

    The only PGN *writing* in the project; parsing is libchess's job via
    gui_helper. Move numbering starts from the start position's own move number
    and side to move, so a game that began from an opening-book position mid-line
    is numbered correctly instead of being renumbered from 1.
    """
    lines = []
    for k in ("Event", "Site", "Date", "Round", "White", "Black", "Result"):
        lines.append('[%s "%s"]' % (k, str(tags.get(k, "?")).replace('"', "'")))
    lines.append('[PlyCount "%d"]' % len(sans))
    for k, v in tags.items():
        if k in ("Event", "Site", "Date", "Round", "White", "Black", "Result"):
            continue
        if v:
            lines.append('[%s "%s"]' % (k, str(v).replace('"', "'")))
    if start_fen and start_fen != START_FEN:
        lines.append('[SetUp "1"]')
        lines.append('[FEN "%s"]' % start_fen)

    parts = start_fen.split() if start_fen else []
    try:
        move_no = int(parts[5]) if len(parts) > 5 else 1
    except ValueError:
        move_no = 1
    white_to_move = (parts[1] == "w") if len(parts) > 1 else True

    body, first = [], True
    for san in sans:
        if white_to_move:
            body.append("%d." % move_no)
        elif first:
            body.append("%d..." % move_no)
        body.append(san)
        first = False
        if not white_to_move:
            move_no += 1
        white_to_move = not white_to_move
    body.append(str(tags.get("Result", "*")))
    return "\n".join(lines) + "\n\n" + " ".join(body) + "\n"


def api_pgn_save_game(body):
    """Save a game played in the Play tab (or any san list) as PGN."""
    sans = body.get("sans")
    if not isinstance(sans, list) or not sans:
        raise ApiError("no moves to save")
    sans = [str(x) for x in sans]
    tags = body.get("tags") or {}
    if not isinstance(tags, dict):
        raise ApiError("'tags' must be an object")
    tags.setdefault("Event", "creatica GUI game")
    tags.setdefault("Site", "local")
    tags.setdefault("Date", time.strftime("%Y.%m.%d"))
    tags.setdefault("Round", "-")
    tags.setdefault("Result", "*")
    start_fen = body.get("start_fen") or START_FEN
    pgn = format_pgn(tags, sans, start_fen)
    name = body.get("name") or ("game_%s.pgn" % time.strftime("%Y%m%d_%H%M%S"))
    return api_pgn_save({"name": name, "pgn": pgn,
                         "append": bool(body.get("append"))})


def _pgn_list_entries():
    seen, out = set(), []
    for root in PGN_ROOTS:
        label = "games" if root == GAMES_DIR else "project"
        try:
            with os.scandir(root) as it:
                for e in it:
                    if not (e.is_file() and e.name.lower().endswith(".pgn")):
                        continue
                    if e.name in seen:
                        continue
                    seen.add(e.name)
                    st = e.stat()
                    out.append({"name": e.name, "bytes": st.st_size,
                                "mtime": int(st.st_mtime), "where": label})
        except FileNotFoundError:
            continue
    out.sort(key=lambda d: -d["mtime"])
    return out


def api_pgn_games(body):
    """List the games inside a saved PGN file.

    Parsing is done by gui_helper, which uses libchess's own initGame(). There is
    deliberately no PGN parser in this file or in the browser: the library already
    handles comments, variations, NAGs and games that start from a FEN, and a second
    parser would drift from what the engine considers a legal game.
    """
    path = _resolve_pgn(body.get("name"))
    # A 12 MB archive holds tens of thousands of games; listing them all would produce
    # a JSON reply far larger than the file. The helper is asked for the lot and the
    # reply is trimmed here, with a flag so the UI can say what it is showing.
    res = HELPER.command("pgnlist " + path)
    games = res.get("games") or []
    cap = 500
    if len(games) > cap:
        res["games"] = games[:cap]
        res["truncated"] = True
        res["shown"] = cap
    return res


def api_pgn_game(body):
    """Expand one game into per-ply san/uci/fen so the UI can step through it."""
    try:
        idx = int(body.get("index") or 0)
    except (TypeError, ValueError):
        raise ApiError("'index' must be an integer")
    path = _resolve_pgn(body.get("name"))
    return HELPER.command("pgngame %d %s" % (idx, path))


# Cache the probe: starting an engine costs a few seconds because it loads a ~110 MB net,
# and the option list does not change between runs of the same binary.
_OPTION_CACHE = {}
_OPTION_CACHE_LOCK = threading.Lock()


def probe_options(name):
    """Start the engine, read its option block, and stop it again.

    Deliberately NOT via the pool. A pooled probe leaves a whole engine process alive --
    a ~110 MB net load and a hash allocation -- purely to have answered one question, and
    it showed up as a third idle `creatica` sitting beside the two a match actually needs.
    The parsed list is cached per binary, so this costs one short-lived process per binary
    per server run.

    It also cannot disturb a match: it never touches the pool, so it can neither evict a
    running engine nor be handed one that is mid-search.
    """
    path = os.path.join(SCRIPT_DIR, name)
    if not os.path.isfile(path):
        raise ApiError("no such engine: %s" % name)
    try:
        proc = subprocess.Popen(
            [path], cwd=SCRIPT_DIR,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1)
    except OSError as exc:
        raise ApiError("could not start %s: %s" % (name, exc))
    lines = []
    try:
        proc.stdin.write("uci\n")
        proc.stdin.flush()
        deadline = time.time() + HANDSHAKE_TIMEOUT
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if line.strip() == "uciok":
                break
            lines.append(line.rstrip())
        else:
            raise ApiError("%s did not answer 'uci' within %.0fs"
                           % (name, HANDSHAKE_TIMEOUT))
        try:
            proc.stdin.write("quit\n")
            proc.stdin.flush()
        except Exception:
            pass
    finally:
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
    return parse_option_block(lines)


def api_engine_options(body):
    """What a given engine advertises in reply to "uci".

    This is what lets the browser build its settings editor from the engine itself instead
    of holding a hand-maintained copy of the names, types and defaults -- which is how the
    weights option came to be missing from the UI for so long.
    """
    name = resolve_engine_name(body.get("engine"))
    with _OPTION_CACHE_LOCK:
        hit = _OPTION_CACHE.get(name)
    if hit is not None:
        return {"ok": True, "engine": name, "options": hit, "cached": True}
    opts = probe_options(name)
    with _OPTION_CACHE_LOCK:
        _OPTION_CACHE[name] = opts
    return {"ok": True, "engine": name, "options": opts, "cached": False}


def api_policy_nets():
    """List exported policy nets beside the script.

    Identified by the header gui-side rather than by filename: a .bin in this directory
    is not necessarily a policy net, and offering the wrong file would fail inside the
    engine at load time with nothing on screen to explain it.
    """
    import struct
    out = []
    try:
        with os.scandir(SCRIPT_DIR) as it:
            for e in it:
                if not (e.is_file() and e.name.lower().endswith(".bin")):
                    continue
                try:
                    with open(e.path, "rb") as fh:
                        hdr = fh.read(28)
                    if len(hdr) < 28:
                        continue
                    magic, ver, nin, h1, h2, nout, conv = struct.unpack("<7i", hdr)
                    if magic != 0x4C4F5043:      # "CPOL"
                        continue
                    out.append({"name": e.name, "in": nin, "h1": h1, "h2": h2,
                                "out": nout, "bytes": e.stat().st_size})
                except Exception:
                    continue
    except FileNotFoundError:
        pass
    out.sort(key=lambda d: d["name"])
    return {"ok": True, "nets": out}


def api_pgn_list():
    return {"ok": True, "games": _pgn_list_entries(), "dir": GAMES_DIR,
            "roots": PGN_ROOTS}


def api_pgn_load(body):
    path = _resolve_pgn(body.get("name"))
    # Raw text, so bound it: this exists for small files, and the parsed form comes
    # from /api/pgn/game instead.
    if os.path.getsize(path) > 4 * 1024 * 1024:
        raise ApiError("file is too large to return as text; open it by game instead")
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return {"ok": True, "name": os.path.basename(path), "pgn": fh.read()}


class Handler(BaseHTTPRequestHandler):
    server_version = "ChessGUI/1.0"
    protocol_version = "HTTP/1.1"

    # Keep-alive is worth having (the UI polls the tournament state twice a
    # second), but an idle connection must not live forever or it would keep a
    # handler thread parked on a blocking read and stall shutdown.
    timeout = 65

    # -- plumbing ---------------------------------------------------------

    def log_request(self, code="-", size="-"):
        """Routine access logging, off by default.

        The Tournament tab polls /api/tournament/state about twice a second, so a match
        buries anything worth reading under thousands of identical 200 lines. Failures are
        still reported: log_error() is untouched, and a non-2xx response is still printed
        by the branch below. Set GUI_ACCESS_LOG=1 to get every request back.
        """
        try:
            numeric = int(code)
        except (TypeError, ValueError):
            numeric = 0
        if os.environ.get("GUI_ACCESS_LOG") or not (200 <= numeric < 400):
            self.log_message('"%s" %s %s', self.requestline, str(code), str(size))

    def log_message(self, fmt, *args):
        # One tidy line on stderr. Reached by log_error() and by the non-2xx branch above.
        sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(),
                                        fmt % args))

    def _send_bytes(self, payload, content_type, code=200):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the UI navigated away mid-response

    def _send_json(self, obj, code=200):
        self._send_bytes(json.dumps(obj).encode("utf-8"),
                         "application/json; charset=utf-8", code)

    def _read_json(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise ApiError("bad Content-Length header")
        if length <= 0:
            return {}
        if length > 4 * 1024 * 1024:
            raise ApiError("request body too large")
        raw = self.rfile.read(length)
        try:
            body = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            raise ApiError("request body is not valid JSON: %s" % exc)
        if not isinstance(body, dict):
            raise ApiError("request body must be a JSON object")
        return body

    def _serve_html(self):
        try:
            with open(HTML_PATH, "rb") as fh:
                data = fh.read()
        except OSError:
            msg = ("chess_gui.html was not found next to chess_gui.py "
                   "(expected at %s)." % HTML_PATH).encode("utf-8")
            self._send_bytes(msg, "text/plain; charset=utf-8", 404)
            return
        self._send_bytes(data, "text/html; charset=utf-8")

    # -- routes -----------------------------------------------------------

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        try:
            if path in ("/", "/index.html", "/chess_gui.html"):
                self._serve_html()
            elif path == "/api/engines":
                self._send_json(api_engines())
            elif path == "/api/pgn/list":
                self._send_json(api_pgn_list())
            elif path == "/api/policy_nets":
                self._send_json(api_policy_nets())
            elif path == "/api/tournament/state":
                qs = parse_qs(parsed.query)
                tid = (qs.get("id") or [None])[0]
                self._send_json(api_tournament_state(tid))
            else:
                self._send_json({"ok": False, "error": "not found: %s" % path},
                                404)
        except ApiError as exc:
            self._send_json({"ok": False, "error": str(exc)})
        except Exception as exc:
            self._send_json({"ok": False, "error": "server error: %s" % exc},
                            500)

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            body = self._read_json()
            if path == "/api/legal":
                self._send_json(api_legal(body))
            elif path == "/api/move":
                self._send_json(api_move(body))
            elif path == "/api/analyse":
                self._send_json(api_analyse(body))
            elif path == "/api/tournament/start":
                self._send_json(api_tournament_start(body))
            elif path == "/api/tournament/stop":
                self._send_json(api_tournament_stop(body))
            elif path == "/api/pgn/save":
                self._send_json(api_pgn_save(body))
            elif path == "/api/pgn/load":
                self._send_json(api_pgn_load(body))
            elif path == "/api/engine_options":
                self._send_json(api_engine_options(body))
            elif path == "/api/pgn/games":
                self._send_json(api_pgn_games(body))
            elif path == "/api/pgn/game":
                self._send_json(api_pgn_game(body))
            elif path == "/api/pgn/save_game":
                self._send_json(api_pgn_save_game(body))
            else:
                self._send_json({"ok": False, "error": "not found: %s" % path},
                                404)
        except ApiError as exc:
            self._send_json({"ok": False, "error": str(exc)})
        except Exception as exc:
            self._send_json({"ok": False, "error": "server error: %s" % exc},
                            500)


# --------------------------------------------------------------------------
# Shutdown
# --------------------------------------------------------------------------

_shutdown_once = threading.Lock()
_shutdown_done = False


def shutdown_all():
    """Stop tournaments, quit every engine, close the helper.  Idempotent."""
    global _shutdown_done
    with _shutdown_once:
        if _shutdown_done:
            return
        _shutdown_done = True

    with TOURNAMENTS_LOCK:
        tours = list(TOURNAMENTS.values())
    for tour in tours:
        tour.stop()
    for tour in tours:
        if tour.thread is not None:
            tour.thread.join(timeout=3.0)

    POOL.shutdown()
    HELPER.shutdown()


atexit.register(shutdown_all)


def main():
    parser = argparse.ArgumentParser(description="libchess GUI server")
    parser.add_argument("--port", type=int, default=8080,
                        help="port to listen on (default 8080)")
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    # Handler threads are daemons, so server_close() must not try to join them:
    # with block_on_close left True, one client holding an idle keep-alive
    # connection open would hang shutdown indefinitely.
    server.daemon_threads = True
    server.block_on_close = False

    # Shut down gracefully on a signal rather than relying on KeyboardInterrupt.
    # Two reasons this matters: SIGTERM (what "kill" sends) would otherwise
    # terminate the process outright, running no atexit handler and orphaning
    # every engine; and a server started as a background job inherits SIGINT
    # set to SIG_IGN, which Python honours, so Ctrl-C style shutdown would
    # never fire either.  server.shutdown() must not be called from the signal
    # handler itself -- it waits for serve_forever(), which is the very thread
    # running the handler -- so hand it to a short-lived thread.
    def request_shutdown(signum, _frame):
        print("\nreceived signal %d, shutting down..." % signum)
        threading.Thread(target=server.shutdown, daemon=True).start()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, request_shutdown)
        except (ValueError, OSError):
            pass  # not the main thread, or the platform disallows it

    print("chess_gui serving on http://127.0.0.1:%d/" % args.port)
    print("  directory : %s" % SCRIPT_DIR)
    print("  gui_helper: %s" % ("found" if os.path.isfile(HELPER_PATH)
                                else "MISSING (%s)" % HELPER_PATH))
    print("  engines   : %s" % (", ".join(list_engines()) or "none found"))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down...")
    finally:
        server.server_close()
        shutdown_all()
        print("stopped cleanly; no engines left running")


if __name__ == "__main__":
    main()
