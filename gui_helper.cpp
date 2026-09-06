// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -O3 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o gui_helper gui_helper.cpp
//
// gui_helper - Component A of the chess GUI.
//
// Reads command lines on stdin, writes exactly ONE line of JSON to stdout per line
// read, flushing after each, and runs until stdin closes.
//
//   legal <FEN>
//     -> {"ok":true,"moves":["e2e4",...],"turn":"w","check":false,"status":"ok"}
//   move <uci> <FEN>
//     -> {"ok":true,"fen":"...","san":"Nf3","turn":"b","check":false,"status":"ok"}
//   anything else, or any failure
//     -> {"ok":false,"error":"..."}
//
// status is one of "ok", "check", "mate", "stalemate".
//
// All move generation, FEN parsing and SAN conversion come from libchess; nothing
// here reimplements them. What IS here is input validation, because libchess's
// fen2board() trusts its caller: it does not check that the position has exactly one
// king per side, that castling rights are backed by a king and a rook that could
// actually castle, or that the en passant field is meaningful. Feeding it a position
// that violates those invariants crashes inside kingMoves()/initCastlingPath()
// rather than returning an error, and this program must never crash on bad input.
// So the FEN is pre-scanned and repaired here, before fen2board() ever sees it.
//
// NNUE is deliberately NOT initialised. This binary never evaluates a position, and
// init_nnue() would cost ~110 MB of net loading on every start for nothing. Only
// Stockfish::Bitboards::init() (required - the sliding attack tables) and
// zobristHash() are called.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <unistd.h>

// nnue/bitboard.h must precede libchess.h: it supplies Stockfish::Bitboard /
// DirtyThreats, which libchess.h's inline helpers use, and libchess.h #errors out
// without it. It is also where Stockfish::Bitboards::init() is declared.
#include "nnue/bitboard.h"
#include "libchess.h"

// ---------------------------------------------------------------- stdout hygiene
//
// The contract says nothing but single JSON lines may reach stdout. Several libchess
// functions print diagnostics with printf() on paths we do not expect to hit
// (board2fen() on an unknown castling rook, reconcile(), move2san() on an illegal
// move). Rather than hope none of them ever fires, fd 1 is pointed at stderr for the
// whole process and the JSON is written to a saved duplicate of the real stdout. Any
// stray library printf() then lands on stderr where it is harmless.
static FILE * jout = nullptr;

static void init_stdout_guard() {
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) { jout = stdout; return; }       // no fds to spare; degrade gracefully
    jout = fdopen(saved, "w");
    if (!jout) { jout = stdout; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
}

static std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += static_cast<char>(c);
        }
    }
    return o;
}

static void emit(const std::string& line) {
    fputs(line.c_str(), jout);
    fputc('\n', jout);
    fflush(jout);
}

static void emit_error(const std::string& reason) {
    emit("{\"ok\":false,\"error\":\"" + json_escape(reason) + "\"}");
}

// ---------------------------------------------------------------- FEN validation
//
// A parsed-but-not-yet-loaded view of the placement field, used to decide whether the
// position is safe to hand to fen2board() and to repair the castling and en passant
// fields.
struct Placement {
    Piece sq[64];
};

static const char * PIECE_CHARS = "PNBRQKpnbrqk";

static bool split_fen(const std::string& fen, std::string f[6], std::string& err) {
    size_t i = 0, n = fen.size(), got = 0;
    while (i < n) {
        while (i < n && isspace((unsigned char)fen[i])) ++i;
        if (i >= n) break;
        size_t start = i;
        while (i < n && !isspace((unsigned char)fen[i])) ++i;
        if (got >= 6) { err = "FEN has more than 6 fields"; return false; }
        f[got++] = fen.substr(start, i - start);
    }
    if (got != 6) { err = "FEN must have 6 space separated fields, got " + std::to_string(got); return false; }
    return true;
}

// Parse the placement field into a mailbox and check the invariants libchess assumes
// but does not verify.
static bool scan_placement(const std::string& board_field, Placement& p, std::string& err) {
    memset(p.sq, PieceNone, sizeof p.sq);
    int rank = 7, file = 0;
    int ranks_seen = 0;
    for (size_t i = 0; i < board_field.size(); ++i) {
        char c = board_field[i];
        if (c == '/') {
            if (file != 8) { err = "FEN rank " + std::to_string(8 - ranks_seen) + " does not describe 8 squares"; return false; }
            ++ranks_seen;
            --rank;
            file = 0;
            if (rank < 0) { err = "FEN placement field has more than 8 ranks"; return false; }
            continue;
        }
        if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8) { err = "FEN rank overflows 8 squares"; return false; }
            continue;
        }
        const char * f = strchr(PIECE_CHARS, c);
        if (!f) { err = std::string("invalid character '") + c + "' in FEN placement field"; return false; }
        if (file > 7) { err = "FEN rank overflows 8 squares"; return false; }
        int s = int(f - PIECE_CHARS);
        // Same encoding fen2board() uses: white pieces are 1..6, black 9..14.
        p.sq[(rank << 3) | file] = s < 6 ? Piece(s + 1) : Piece(s + 3);
        ++file;
    }
    if (file != 8) { err = "FEN rank 1 does not describe 8 squares"; return false; }
    ++ranks_seen;
    if (ranks_seen != 8) { err = "FEN placement field must have 8 ranks, got " + std::to_string(ranks_seen); return false; }

    int wk = 0, bk = 0, wks = -1, bks = -1;
    for (int s = 0; s < 64; ++s) {
        Piece pc = p.sq[s];
        if (pc == PieceNone) continue;
        if (pc == WhiteKing) { ++wk; wks = s; }
        if (pc == BlackKing) { ++bk; bks = s; }
        if (PC_TYPE(pc) == Pawn && (s < 8 || s >= 56)) { err = "illegal position: pawn on the first or last rank"; return false; }
    }
    // kingSquare() is lsBit() over an empty bitboard when a king is missing, and
    // initCastlingPath() indexes BetweenBB with the result - so this must be rejected
    // before fen2board() runs, not after.
    if (wk != 1) { err = "illegal position: expected exactly one white king, found " + std::to_string(wk); return false; }
    if (bk != 1) { err = "illegal position: expected exactly one black king, found " + std::to_string(bk); return false; }
    // Touching kings. getCheckers() does not count the enemy king as a checker, so the
    // "side not to move is in check" guard in load_board() lets this through, and the
    // generator then offers the capture of the opposing king as a legal move.
    {
        int df = abs((wks & 7) - (bks & 7)), dr = abs((wks >> 3) - (bks >> 3));
        if (df <= 1 && dr <= 1) { err = "illegal position: the two kings are adjacent"; return false; }
    }
    return true;
}

// Drop castling letters that no king-and-rook pair could honour. libchess derives the
// castling path from whatever rooks it finds, so an unbacked right does not error out,
// it generates a bogus castling move (or walks off BetweenBB). Repairing the FEN text
// before fen2board() means initCastlingPath() is computed from the repaired rights.
// ASSUMPTION: repairing is preferable to rejecting. A stale castling right is a common
// bug in FENs pasted into GUIs, and the position is still perfectly playable without it.
static std::string sanitize_castling(const std::string& field, const Placement& p) {
    if (field == "-") return field;
    std::string out;
    for (char c : field) {
        bool keep = false;
        switch (c) {
        case 'K': keep = p.sq[SquareE1] == WhiteKing && p.sq[SquareH1] == WhiteRook; break;
        case 'Q': keep = p.sq[SquareE1] == WhiteKing && p.sq[SquareA1] == WhiteRook; break;
        case 'k': keep = p.sq[SquareE8] == BlackKing && p.sq[SquareH8] == BlackRook; break;
        case 'q': keep = p.sq[SquareE8] == BlackKing && p.sq[SquareA8] == BlackRook; break;
        default:
            // Shredder-FEN / Chess960 form: a file letter naming the castling rook.
            if (c >= 'A' && c <= 'H') {
                int f = c - 'A';
                keep = p.sq[f] == WhiteRook;
                if (keep) {   // and the white king must share the back rank
                    bool k = false;
                    for (int s = SquareA1; s <= SquareH1; ++s) if (p.sq[s] == WhiteKing) k = true;
                    keep = k;
                }
            } else if (c >= 'a' && c <= 'h') {
                int f = c - 'a';
                keep = p.sq[56 + f] == BlackRook;
                if (keep) {
                    bool k = false;
                    for (int s = SquareA8; s <= SquareH8; ++s) if (p.sq[s] == BlackKing) k = true;
                    keep = k;
                }
            }
            break;
        }
        if (keep) out += c;
    }
    return out.empty() ? "-" : out;
}

// An en passant file that no pawn actually created makes legalEnPassantMoveFromSq()
// invent a capture of a pawn that is not there, which do_move() then xors out of the
// bitboards. Reduce anything that is not a real double push to "-".
static std::string sanitize_ep(const std::string& field, const Placement& p, bool white_to_move, std::string& err) {
    if (field == "-") return field;
    if (field.size() != 2 || field[0] < 'a' || field[0] > 'h' || field[1] < '1' || field[1] > '8') {
        err = "invalid en passant field '" + field + "'";
        return "";
    }
    int f = field[0] - 'a';
    char want_rank = white_to_move ? '6' : '3';
    if (field[1] != want_rank) return "-";           // wrong rank for the side to move
    int target = white_to_move ? (40 + f) : (16 + f);          // the ep destination square
    int pawn   = white_to_move ? (32 + f) : (24 + f);          // the pawn that just moved two
    int origin = white_to_move ? (48 + f) : (8 + f);           // where it came from
    if (p.sq[pawn] != (white_to_move ? BlackPawn : WhitePawn)) return "-";
    if (p.sq[target] != PieceNone || p.sq[origin] != PieceNone) return "-";
    return field;
}

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    return true;
}

// Validate, repair, and load. Returns false with err set on anything unusable.
static bool load_board(const std::string& fen_in, Board& board, std::string& err) {
    std::string f[6];
    if (!split_fen(fen_in, f, err)) return false;

    Placement p;
    if (!scan_placement(f[0], p, err)) return false;

    if (f[1] != "w" && f[1] != "b") { err = "side to move must be 'w' or 'b', got '" + f[1] + "'"; return false; }
    const bool white_to_move = (f[1] == "w");

    if (f[2].size() > 4) { err = "castling field is longer than 4 characters"; return false; }
    for (char c : f[2]) {
        if (c == '-') continue;
        if (!strchr("KQkqABCDEFGHabcdefgh", c)) { err = std::string("invalid character '") + c + "' in castling field"; return false; }
    }
    f[2] = sanitize_castling(f[2], p);

    f[3] = sanitize_ep(f[3], p, white_to_move, err);
    if (f[3].empty()) return false;

    // board.halfmoveClock is a uint8_t and board.moveNumber an int; out of range values
    // would silently wrap rather than be reported.
    if (!all_digits(f[4])) { err = "halfmove clock must be a number, got '" + f[4] + "'"; return false; }
    if (strtoul(f[4].c_str(), nullptr, 10) > 255) { err = "halfmove clock out of range (0-255)"; return false; }
    if (!all_digits(f[5])) { err = "move number must be a number, got '" + f[5] + "'"; return false; }
    unsigned long mn = strtoul(f[5].c_str(), nullptr, 10);
    if (mn < 1 || mn > 1000000) { err = "move number out of range (1-1000000)"; return false; }

    std::string fen = f[0] + " " + f[1] + " " + f[2] + " " + f[3] + " " + f[4] + " " + f[5];
    if (fen.size() + 1 > MAX_FEN_STRING_LEN) { err = "FEN is too long"; return false; }

    board = Board{};                       // never inherit mailbox state from the last call
    if (fen2board(board, fen.c_str())) { err = "could not parse FEN"; return false; }

    // The side NOT to move must not be in check; that position is unreachable and the
    // legal move generator is not defined on it (it would generate a king capture).
    {
        Board probe = board;
        Color them = OPP_COLOR(board.sideToMove);
        probe.sideToMove = them;
        if (getCheckers(probe, kingSquare(probe, them))) {
            err = "illegal position: the side not to move is in check";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- move generation
//
// The canonical staged, pin-aware, legal-only loop from CLAUDE.md, exactly as
// bench_prior_acc.cpp writes it. kingMoves() is what sets board.isCheck, so callers
// can read that flag after this returns.
static void generate(Board& board, std::vector<Move>& out) {
    out.clear();
    auto [kingBB, pinned, pinning, checkers, kingSq] = kingMoves(board);
    Move move = {};
    move.src = kingSq;
    move.promoType = PieceTypeNone;
    for (uint64_t m = kingBB; m; m &= m - 1) {
        move.dst = lsBit(m);
        out.push_back(move);
    }
    if (bitCount(checkers) > 1) return;                    // double check: king moves only
    auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSq, checkers)
                                          : std::make_pair(~0ULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
        uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
        while (occ) {
            move.src = lsBit(occ);
            uint64_t mv = piece_moves(board, pt, move.src, kingSq, pinned, pinning, check_mask, ep_mask);
            while (mv) {
                move.dst = lsBit(mv);
                if (promoMove(board, move)) {
                    for (PieceType q = Knight; q <= Queen; q = PieceType(q + 1)) {
                        move.promoType = q;
                        out.push_back(move);
                    }
                    move.promoType = PieceTypeNone;
                } else {
                    move.promoType = PieceTypeNone;
                    out.push_back(move);
                }
                mv &= mv - 1;
            }
            occ &= occ - 1;
        }
    }
}

static std::string to_uci(const Move& m) {
    std::string s = std::string(square[m.src]) + square[m.dst];
    // uciPromoLetter is '\0' for everything that is not a promotion piece.
    // idx2uci() is not used here: it writes the promotion letter into index 4 without
    // terminating index 5.
    if (m.promoType != PieceTypeNone && uciPromoLetter[m.promoType])
        s += uciPromoLetter[m.promoType];
    return s;
}

// "ok" / "check" / "mate" / "stalemate" for whoever is to move on `board`.
// isCheckMateStaleMate() computes the same thing, but it CLEARS board.isCheck when it
// sets isMate, and the API contract wants both the status and the check flag, so the
// move list is used directly instead.
static const char * status_of(const Board& board, const std::vector<Move>& moves) {
    if (moves.empty()) return board.isCheck ? "mate" : "stalemate";
    return board.isCheck ? "check" : "ok";
}

// ---------------------------------------------------------------------- commands
static void cmd_legal(const std::string& fen) {
    Board board;
    std::string err;
    if (!load_board(fen, board, err)) { emit_error(err); return; }

    std::vector<Move> moves;
    generate(board, moves);
    const char * status = status_of(board, moves);

    std::string out = "{\"ok\":true,\"moves\":[";
    // A mated or stalemated side has no moves, so the list is naturally empty.
    for (size_t i = 0; i < moves.size(); ++i) {
        if (i) out += ",";
        out += "\"" + to_uci(moves[i]) + "\"";
    }
    out += "],\"turn\":\"";
    out += (board.sideToMove == ColorWhite ? "w" : "b");
    // ASSUMPTION: "check" means "the side to move is in check", so it is true for mate
    // as well as for a plain check.
    out += "\",\"check\":";
    out += (board.isCheck || !strcmp(status, "mate")) ? "true" : "false";
    out += ",\"status\":\"";
    out += status;
    out += "\"}";
    emit(out);
}

static void cmd_move(const std::string& uci, const std::string& fen) {
    Board board;
    std::string err;
    if (!load_board(fen, board, err)) { emit_error(err); return; }

    if (uci.size() < 4 || uci.size() > 5 ||
        uci[0] < 'a' || uci[0] > 'h' || uci[1] < '1' || uci[1] > '8' ||
        uci[2] < 'a' || uci[2] > 'h' || uci[3] < '1' || uci[3] > '8' ||
        (uci.size() == 5 && !strchr("nbrq", uci[4]))) {
        emit_error("malformed move '" + uci + "'");
        return;
    }
    Move wanted = {};
    uci2move_idx(uci.c_str(), wanted);

    std::vector<Move> moves;
    generate(board, moves);

    const Move * chosen = nullptr;
    for (const Move& m : moves) {
        if (m.src != wanted.src || m.dst != wanted.dst) continue;
        // ASSUMPTION: a promotion sent without its piece letter means a queen. Some GUIs
        // send the bare "e7e8"; rejecting it would be the stricter but less useful read.
        PieceType want = wanted.promoType;
        if (want == PieceTypeNone && m.promoType != PieceTypeNone) want = Queen;
        if (m.promoType != want) continue;
        chosen = &m;
        break;
    }
    if (!chosen) { emit_error("illegal move '" + uci + "' in this position"); return; }

    Move move = *chosen;

    // move2san() reads move.type to decide whether to write 'x' or "O-O", and it needs
    // the PRE-move board for disambiguation, so the type has to be classified here:
    // do_move() sets it, but only after the SAN would have to be written.
    if (board.piecesOnSquares[move.dst] != PieceNone) move.type = MoveTypeCapture;
    else if (PC_TYPE(board.piecesOnSquares[move.src]) == Pawn &&
             SQ_FILE(move.src) != SQ_FILE(move.dst)) {
        // A diagonal pawn move to an empty square is en passant. move2san() only writes
        // the "exd6" form for MoveTypeCapture, so it is labelled a capture here; do_move()
        // reclassifies it to MoveTypeEnPassantCapture on its own.
        move.type = MoveTypeCapture;
    }
    if (!board.isChess960 && PC_TYPE(board.piecesOnSquares[move.src]) == King) {
        const int d = int(move.dst) - int(move.src);
        if (d == 2) move.type = MoveTypeCastlingKingside;
        else if (d == -2) move.type = MoveTypeCastlingQueenside;
    }

    char sanBuf[32] = "";
    const char * sanp = move2san(board, move, sanBuf);
    std::string san = sanp ? sanp : to_uci(move);

    StateInfo state = {};
    do_move(board, move, state);

    std::vector<Move> replies;
    generate(board, replies);
    const char * status = status_of(board, replies);
    // move2san() stops before the check and mate markers; standard SAN carries them.
    if (!strcmp(status, "mate")) san += "#";
    else if (board.isCheck) san += "+";

    char fenBuf[MAX_FEN_STRING_LEN] = "";
    board2fen(board, fenBuf);

    std::string out = "{\"ok\":true,\"fen\":\"";
    out += json_escape(fenBuf);
    out += "\",\"san\":\"";
    out += json_escape(san);
    out += "\",\"turn\":\"";
    out += (board.sideToMove == ColorWhite ? "w" : "b");
    out += "\",\"check\":";
    out += (board.isCheck || !strcmp(status, "mate")) ? "true" : "false";
    out += ",\"status\":\"";
    out += status;
    out += "\"}";
    emit(out);
}

// -------------------------------------------------------------------------- main
// ------------------------------------------------------------------- PGN
//
// PGN parsing comes from libchess (initGame / san2move / the Tags enum), not from a
// hand-written parser here or in the browser. Real PGN carries comments, variations,
// NAGs and inconsistent whitespace, and a second parser in another language would
// diverge from what the rest of the project considers a legal game. play_games.cpp is
// the canonical example of this loop.

static const char * START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// A tag value, or "" when the tag is absent.
static std::string tagval(const Game& g, Tags t) {
    return std::string(g.tags[t]);
}

static std::string game_tags_json(const Game& g) {
    static const Tags WANT[] = { Event, Site, Date, Round, White, Black, Result,
                                 ECO, Opening, Variation, WhiteElo, BlackElo,
                                 TimeControl, Termination, FEN };
    std::string out = "{";
    bool first = true;
    for (Tags t : WANT) {
        const std::string v = tagval(g, t);
        if (v.empty()) continue;
        if (!first) out += ",";
        first = false;
        out += "\"" + std::string(tags[t]) + "\":\"" + json_escape(v) + "\"";
    }
    return out + "}";
}

// Safety net, not a workaround any more.
//
// initGame() used to return early on EOF, BEFORE stripping the result, comments,
// variations and move numbers, so the LAST game of a file came back raw with
// numberOfPlies left at 0 -- unless the file happened to end with a blank line. That is
// fixed in pgn.cpp now: normalisation happens first and the EOF status is returned
// afterwards. This stays because gui_helper can be run against an older libchess.dylib,
// where it repairs the raw game instead of silently feeding "1.e4" to san2move(). With a
// current library it never fires.
static void finish_if_raw(Game& g) {
    if (g.numberOfPlies > 0 || g.sanMoves[0] == '\0') return;
    stripGameResult(g);
    normalizeMoves(g.sanMoves);
    g.numberOfPlies = movesOnly(g.sanMoves);
}

static std::vector<std::string> split_ws(const char * s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char * p = s; *p; ++p) {
        if (isspace((unsigned char)*p)) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += *p;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void cmd_pgnlist(const std::string& path) {
    FILE * f = fopen(path.c_str(), "r");
    if (!f) { emit_error("cannot open '" + path + "'"); return; }
    std::string out = "{\"ok\":true,\"games\":[";
    int n = 0;
    while (true) {
        Game game;
        const int res = initGame(game, f);
        // initGame() returns non-zero when it reaches EOF, INCLUDING the call that
        // successfully read the final game -- so a non-zero return does not mean "no
        // game". Treating it as one silently dropped the last game of every file: one
        // game read as zero, two as one, forty-two as forty-one. play_games.cpp has the
        // same shape and uses the game regardless, which is the correct reading.
        finish_if_raw(game);
        const bool has_game = (game.numberOfPlies > 0) || game.tags[White][0] ||
                              game.tags[Event][0];
        if (!has_game) break;
        if (n) out += ",";
        out += "{\"index\":" + std::to_string(n) +
               ",\"plies\":" + std::to_string(game.numberOfPlies) +
               ",\"tags\":" + game_tags_json(game) + "}";
        ++n;
        if (res || feof(f)) break;
    }
    fclose(f);
    out += "],\"count\":" + std::to_string(n) + "}";
    emit(out);
}

// Expand one game into a per-ply list the UI can step through without re-deriving
// anything: SAN as written, the UCI move, and the FEN after it.
static void cmd_pgngame(int want, const std::string& path) {
    FILE * f = fopen(path.c_str(), "r");
    if (!f) { emit_error("cannot open '" + path + "'"); return; }
    Game game;
    bool found = false;
    int n = 0;
    while (true) {
        Game g;
        const int res = initGame(g, f);
        finish_if_raw(g);
        const bool has_game = (g.numberOfPlies > 0) || g.tags[White][0] || g.tags[Event][0];
        if (!has_game) break;
        if (n == want) { game = g; found = true; break; }
        ++n;
        if (res || feof(f)) break;
    }
    fclose(f);
    if (!found) { emit_error("no game at index " + std::to_string(want)); return; }

    const std::string startFen = tagval(game, FEN).empty() ? std::string(START_FEN)
                                                           : tagval(game, FEN);
    Board board = {};
    std::string err;
    if (!load_board(startFen, board, err)) {
        emit_error("game " + std::to_string(want) + " start position: " + err);
        return;
    }

    std::string out = "{\"ok\":true,\"index\":" + std::to_string(want) +
                      ",\"tags\":" + game_tags_json(game) +
                      ",\"start_fen\":\"" + json_escape(startFen) + "\",\"moves\":[";
    int applied = 0;
    std::string stopped;
    for (const std::string& tok : split_ws(game.sanMoves)) {
        // The result token can be left on the move list by some writers.
        if (tok == "1-0" || tok == "0-1" || tok == "1/2-1/2" || tok == "*") continue;
        Move mv = {};
        if (san2move(board, tok.c_str(), mv)) {
            stopped = "could not interpret '" + tok + "' at ply " + std::to_string(applied + 1);
            break;
        }
        char sanBuf[32] = "";
        const char * sanp = move2san(board, mv, sanBuf);
        const std::string sanOut = sanp ? sanp : tok;
        StateInfo st = {};
        do_move(board, mv, st);
        isCheckMateStaleMate(board);
        char fenBuf[MAX_FEN_STRING_LEN] = "";
        const char * fp = board2fen(board, fenBuf);
        if (applied) out += ",";
        out += "{\"san\":\"" + json_escape(sanOut) + "\",\"uci\":\"" + to_uci(mv) +
               "\",\"fen\":\"" + json_escape(fp ? fp : "") + "\"}";
        ++applied;
    }
    out += "],\"plies\":" + std::to_string(applied);
    if (!stopped.empty()) out += ",\"warning\":\"" + json_escape(stopped) + "\"";
    out += "}";
    emit(out);
}

int main() {
    init_stdout_guard();

    Stockfish::Bitboards::init();          // REQUIRED: sliding attack tables
    Zobrist z = {};
    zobristHash(z);                        // cheap, and keeps the documented init order

    std::string line;
    // Read with getline on the C stdin so this stays a plain line protocol.
    {
        char * buf = nullptr;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&buf, &cap, stdin)) > 0) {
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
            line.assign(buf, size_t(n));

            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) {
                // ASSUMPTION: one line in, one line out, blank lines included, so the
                // caller's read never blocks waiting for a reply that is not coming.
                emit_error("empty command");
                continue;
            }
            size_t e = line.find_first_of(" \t", b);
            std::string cmd = line.substr(b, e == std::string::npos ? std::string::npos : e - b);
            std::string rest = (e == std::string::npos) ? "" : line.substr(e + 1);
            // Leading blanks only; the FEN itself contains spaces and is the rest of the line.
            size_t rb = rest.find_first_not_of(" \t");
            rest = (rb == std::string::npos) ? "" : rest.substr(rb);

            if (cmd == "legal") {
                if (rest.empty()) emit_error("legal: missing FEN");
                else cmd_legal(rest);
            } else if (cmd == "move") {
                size_t sp = rest.find_first_of(" \t");
                if (sp == std::string::npos) { emit_error("move: expected 'move <uci> <FEN>'"); continue; }
                std::string uci = rest.substr(0, sp);
                size_t fb = rest.find_first_not_of(" \t", sp);
                if (fb == std::string::npos) { emit_error("move: missing FEN"); continue; }
                cmd_move(uci, rest.substr(fb));
            } else if (cmd == "pgnlist") {
                if (rest.empty()) emit_error("pgnlist: missing path");
                else cmd_pgnlist(rest);
            } else if (cmd == "pgngame") {
                const size_t sp = rest.find(' ');
                if (sp == std::string::npos) {
                    emit_error("pgngame: expected 'pgngame <index> <path>'");
                } else {
                    const std::string idx = rest.substr(0, sp);
                    const std::string path = rest.substr(sp + 1);
                    if (path.empty()) emit_error("pgngame: missing path");
                    else cmd_pgngame(atoi(idx.c_str()), path);
                }
            } else {
                emit_error("unknown command '" + cmd + "'");
            }
        }
        free(buf);
    }
    return 0;
}
