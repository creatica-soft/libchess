// compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o minimax minimax.cpp
#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/nnue/nnue_architecture.h"
#include "nnue/nnue/features/half_ka_v2_hm.h"
#include <cstdint>
#include <cassert>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
//#include "tbprobe.h"
#include "libchess-new.h"

#ifdef __cplusplus
extern "C" {
#endif

const int INF = 1000000000;
#define SYZYGY_PATH "/Users/ap/syzygy"
#define TT_SIZE 1024 //in MB

struct NNUEContext {
    Stockfish::StateInfo * state;
    Stockfish::Position * pos;
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};

void init_nnue(const char * nnue_file_big = EvalFileDefaultNameBig, const char * nnue_file_small = EvalFileDefaultNameSmall);
void cleanup_nnue();
void init_nnue_context(struct NNUEContext * ctx);
void free_nnue_context(struct NNUEContext * ctx);
double evaluate_nnue(struct Board * board, struct Move * move, struct NNUEContext * ctx);

#ifdef __cplusplus
}
#endif

unsigned long long nodes = 0;

// Helper for piece value (centipawns, abs for color)
int getPieceValue(int pieceName) {
    int type = pieceName;  // Assume signed for color
    switch (type) {
        case Pawn: return 100;
        case Knight: return 320;
        case Bishop: return 330;
        case Rook: return 500;
        case Queen: return 900;
        case King: return 20000;
        default: return 0;
    }
}

constexpr int8_t TT_EXACT = 0;
constexpr int8_t TT_LOWER = 1;
constexpr int8_t TT_UPPER = 2;

// ---------------------------------------------------------------------
//  Packed TTEntry: 10 bytes
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct TTEntry {
    uint32_t key32       = 0;      // 4 bytes (lower 32 bit of 64-bit Zobrist hash2)
    uint16_t move16      = 0;      // 2
    int16_t  score       = 0;      // 2
    uint8_t  depth_flag  = 0;      // 1 → depth(6) + flag(2)
    uint8_t  age         = 0;      // 1
    // Total: 10 bytes
};
#pragma pack(pop)

static_assert(sizeof(TTEntry) == 10, "TTEntry must be 10 bytes");

// ---------------------------------------------------------------------
//  TTCluster: 6 × 10 = 60 bytes + 4-byte pad = 64 bytes
// ---------------------------------------------------------------------
struct TTCluster {
    TTEntry entries[6];
    uint8_t _padding[4] = {};  // Force 64-byte alignment
};
static_assert(sizeof(TTCluster) == 64, "TTCluster must be 64 bytes");

inline uint16_t pack_move(int src, int dst, int promo) {
    return (src & 63) | ((dst & 63) << 6) | ((promo & 15) << 12);
}

inline void unpack_move(uint16_t move16, int& src, int& dst, int& promo) {
    src   = move16 & 63;
    dst   = (move16 >> 6) & 63;
    promo = (move16 >> 12) & 15;
}

inline uint8_t pack_depth_flag(int depth, int flag) {
    return (depth & 63) | ((flag & 3) << 6);
}

inline int unpack_depth(uint8_t df) { return df & 63; }
inline int unpack_flag(uint8_t df)  { return (df >> 6) & 3; }

class TransTable {
public:
    std::vector<TTCluster> table;
    size_t size = 0;
    uint8_t current_age = 0;

    explicit TransTable(size_t mb_size = 16) {
        size_t bytes    = mb_size * 1024ULL * 1024ULL;
        size_t clusters = bytes / 64;
        size = clusters ? (1ULL << (63 - __builtin_clzll(clusters))) : 1;
        table.resize(size);
    }

    size_t index(uint64_t hash) const {
        return hash & (size - 1);
    }

    void new_search() {
        ++current_age;
    }

    void store(uint64_t hash, uint64_t hash2, int score, int depth, int flag, int src, int dst, int promo, int ply) {
        size_t cidx = index(hash);
        TTCluster& cluster = table[cidx];

        TTEntry* replace = nullptr;
        int best_score = INT_MAX;

        uint32_t key32 = static_cast<uint32_t>(hash2);
        uint16_t move16 = pack_move(src, dst, promo);
        uint8_t  depth_flag = pack_depth_flag(depth, flag);

        for (int i = 0; i < 6; ++i) {
            TTEntry& e = cluster.entries[i];

            if (e.key32 == key32) {
                replace = &e;
                break;
            }
            if (e.key32 == 0) {
                replace = &e;
                break;
            }

            int age_diff = (current_age - e.age) & 0xFF;
            int score_val = (unpack_depth(e.depth_flag) << 3) + age_diff;
            if (score_val < best_score) {
                best_score = score_val;
                replace = &e;
            }
        }

        if (score > MATE_SCORE - 100) score -= ply;
        else if (score < -MATE_SCORE + 100) score += ply;

        replace->key32      = key32;
        replace->move16     = move16;
        replace->score      = static_cast<int16_t>(score);
        replace->depth_flag = depth_flag;
        replace->age        = current_age;
    }

    bool probe(uint64_t hash, uint64_t hash2, int depth, int& alpha, int& beta, int& score, int& src, int& dst, int& promo, int ply_num) {
        size_t cidx = index(hash);
        const TTCluster& cluster = table[cidx];

        uint32_t key32 = static_cast<uint32_t>(hash2);

        for (int i = 0; i < 6; ++i) {
            const TTEntry& e = cluster.entries[i];
            if (e.key32 != key32) continue;
            if (unpack_depth(e.depth_flag) < depth) continue;

            score = e.score;
            if (score > MATE_SCORE - 100) score += ply_num;
            else if (score < -MATE_SCORE + 100) score -= ply_num;

            unpack_move(e.move16, src, dst, promo);
            int flag = unpack_flag(e.depth_flag);

            if (flag == TT_EXACT) {
                alpha = beta = score;
                return true;
            }
            if (flag == TT_LOWER && score >= beta) {
                alpha = score;
                return true;
            }
            if (flag == TT_UPPER && score <= alpha) { 
                beta = score;
                return true;
            }
            return false; // Move is set, but no cutoff
        }
        return false;
    }

    void clear() {
        for (auto& c : table) {
            for (auto& e : c.entries) {
                e = TTEntry{};
            }
        }
        current_age = 0;
    }
};

TransTable tt(TT_SIZE);
int ply = 0;
struct SimpleMove {
    int src, dst, promo, score;
};

std::vector<SimpleMove> getSortedMoves(struct Board * board, bool only_quiescence = false) {
    std::vector<SimpleMove> movelist;
    int side = PC(board->fen->sideToMove, PieceTypeAny);
    unsigned long long any = board->occupations[side];
    while (any) {
        int src = lsBit(any);
        unsigned long long moves = board->movesFromSquares[src];
        while (moves) {
            int dst = lsBit(moves);
            bool is_promo = promoMove(board, src, dst);
            bool is_capture = (board->piecesOnSquares[dst] != PieceNameNone);
            if (only_quiescence && !is_capture && !is_promo) {
                moves &= moves - 1; continue;
            }
            int base_score = 0;
            if (is_capture) {
                int victim_val = getPieceValue(board->piecesOnSquares[dst]);
                int attacker_val = getPieceValue(board->piecesOnSquares[src]);
                base_score = (victim_val / 100) * 6 - (attacker_val / 100);  // MVV-LVA simplified
            } else if (!is_promo) {
                // Quiet move bonus: +5 for central pawns (e4/d4/e5/d5 files 3-4)
                int file_bonus = (abs((dst % 8) - 3) <= 1 || abs((dst % 8) - 4) <= 1) ? 5 : 0;
                base_score = file_bonus;
            }
            if (is_promo) {
                for (int p = Knight; p <= Queen; ++p) {
                    movelist.push_back({src, dst, p, base_score + p * 10});
                }
            } else {
                movelist.push_back({src, dst, PieceTypeNone, base_score});
            }
            moves &= moves - 1;
        }
        any &= any - 1;
    }
    std::sort(movelist.begin(), movelist.end(), [](const SimpleMove& a, const SimpleMove& b) {
        return a.score > b.score;
    });
    return movelist;
}

int quiescence(struct Board * board, int qdepth, int alpha, int beta, int original_color, struct NNUEContext *ctx) {
    int orig_alpha = alpha, orig_beta = beta;
    int ply_num = PLY_NUM(board->fen) - ply;
    bool is_max = (board->fen->sideToMove == original_color);
    int sign = is_max ? 1 : -1;

    int tt_score; 
    int tt_src = SquareNone, tt_dst = SquareNone, tt_promo = PieceTypeNone;
    if (tt.probe(board->zh->hash, board->zh->hash2, qdepth, alpha, beta, tt_score, tt_src, tt_dst, tt_promo, ply_num)) {
        return tt_score;
    }
    
    generateMoves(board);
    if (board->isMate) return sign * (-MATE_SCORE);
    if (board->isStaleMate) return 0.0;

    updateFen(board);
    //evaluate_nnue() return result in pawns from the side to move perspective
    int score = static_cast<int>(100 * evaluate_nnue(board, nullptr, ctx));
    nodes++;
    int stand_pat = sign * score;
    if (stand_pat + 900 < alpha) return alpha; //This prunes if even capturing a queen wouldn't help
    if (stand_pat - 900 > beta) return beta; // Is this correct?
    int best_src = SquareNone, best_dst = SquareNone, best_promo = PieceTypeNone;

    if (is_max) {
        if (stand_pat >= beta) return beta;
        if (stand_pat > alpha) alpha = stand_pat;
    } else {
        if (stand_pat <= alpha) return alpha;
        if (stand_pat < beta) beta = stand_pat;
    }
    if (qdepth == 0) return stand_pat;

    auto movelist = getSortedMoves(board, true);
    for (auto& sm : movelist) {
        struct Move move;
        init_move(&move, board, sm.src, sm.dst, sm.promo);
        make_move(&move);
        //generateMoves(board);
        unsigned long long prevHash = board->zh->hash, 
                           prevHash2 = board->zh->hash2, 
                           prevEnPassant = board->zh->prevEnPassant, 
                           prevEnPassant2 = board->zh->prevEnPassant2, 
                           prevCastlingRights = board->zh->prevCastlingRights, 
                           prevCastlingRights2 = board->zh->prevCastlingRights2;        
        updateHash(board, &move);
        int eval = quiescence(board, qdepth - 1, alpha, beta, original_color, ctx);
        undo_move(&move);
        board->zh->hash = prevHash;
        board->zh->hash2 = prevHash2; 
        board->zh->prevEnPassant = prevEnPassant;
        board->zh->prevEnPassant2 = prevEnPassant2;
        board->zh->prevCastlingRights = prevCastlingRights;
        board->zh->prevCastlingRights2 = prevCastlingRights2;
        if (is_max) {
            if (eval > stand_pat) {
                stand_pat = eval;
                best_src = sm.src; best_dst = sm.dst; best_promo = sm.promo;
            }
            if (stand_pat >= beta) break;
            if (stand_pat > alpha) alpha = stand_pat; //raise alpha
        } else {
            if (eval < stand_pat) {
                stand_pat = eval;
                best_src = sm.src; best_dst = sm.dst; best_promo = sm.promo;
            }
            if (stand_pat <= alpha) break;
            if (stand_pat < beta) beta = stand_pat; //lower beta
        }
    }
    //TT_UPPER means upper bound, i.e. we failed low, no move improved beyond alpha
    //TT_LOWER means lower bound, i.e. we failed high, position too good, the move is better than beta
    //TT_EXACT means alpha = beta = stand_pat
    int flag = (stand_pat <= orig_alpha) ? TT_UPPER : (stand_pat >= orig_beta ? TT_LOWER : TT_EXACT);
    tt.store(board->zh->hash, board->zh->hash2, stand_pat, qdepth, flag, best_src, best_dst, best_promo, ply_num);
    return stand_pat;
}

int alphaBetaMinimax(struct Board * board, int depth, int alpha, int beta, int original_color, struct NNUEContext * ctx) {
    int orig_alpha = alpha, orig_beta = beta;
    int ply_num = PLY_NUM(board->fen) - ply;
    bool is_max = (board->fen->sideToMove == original_color);
    int sign = is_max ? 1 : -1;
    nodes++;

    int tt_score; int tt_src = SquareNone, tt_dst = SquareNone, tt_promo = PieceTypeNone;
    if (tt.probe(board->zh->hash, board->zh->hash2, depth, alpha, beta, tt_score, tt_src, tt_dst, tt_promo, ply_num)) {
        return tt_score;  //the position is already evaluated because of transposition or repetition, so return its score
    }
    
    generateMoves(board); 
    if (board->isMate) return sign * (-MATE_SCORE); //terminate position
    if (board->isStaleMate) return 0.0; //terminate position

    if (depth == 0) {
        return quiescence(board, 3, alpha, beta, original_color, ctx);
    }

    if (ply_num > 64) {
        updateFen(board);  
        return sign * static_cast<int>(100 * evaluate_nnue(board, nullptr, ctx));
    }
    
    int bestEval = sign * (-INF);
    int best_src = SquareNone, best_dst = SquareNone, best_promo = PieceTypeNone;

    auto movelist = getSortedMoves(board);

    for (auto& sm : movelist) {
        struct Move move;
        init_move(&move, board, sm.src, sm.dst, sm.promo);
        make_move(&move);
        unsigned long long prevHash = board->zh->hash, 
                           prevHash2 = board->zh->hash2, 
                           prevEnPassant = board->zh->prevEnPassant, 
                           prevEnPassant2 = board->zh->prevEnPassant2, 
                           prevCastlingRights = board->zh->prevCastlingRights, 
                           prevCastlingRights2 = board->zh->prevCastlingRights2;        
        updateHash(board, &move);
        //recursion
        //it terminates when depth reaches 0 (reverse depth counting here), in terminal position or tt hit
        int eval = alphaBetaMinimax(board, depth - 1, alpha, beta, original_color, ctx);
        undo_move(&move);
        board->zh->hash = prevHash;
        board->zh->hash2 = prevHash2; 
        board->zh->prevEnPassant = prevEnPassant;
        board->zh->prevEnPassant2 = prevEnPassant2;
        board->zh->prevCastlingRights = prevCastlingRights;
        board->zh->prevCastlingRights2 = prevCastlingRights2;
        if (is_max) {
            if (eval > bestEval) {
                bestEval = eval;
                best_src = sm.src; best_dst = sm.dst; best_promo = sm.promo;
            }
            if (eval > alpha) alpha = eval; //alpha is the lower bound on the max player's guaranteed score (raised as better options found)
        } else {
            if (eval < bestEval) {
                bestEval = eval;
                best_src = sm.src; best_dst = sm.dst; best_promo = sm.promo;
            }
            if (eval < beta) beta = eval; //beta is the upper bound on the min player's guaranteed score (lowered as worse options for max are found)
        }
        if (beta <= alpha) break; //the alpha-beta cutoff: If alpha >= beta, further moves in this branch can't improve the result (for max: we've found something at least as good as beta, so min wouldn't allow it; symmetric for min). It doesn't mean "best move found" overall, just prune this branch. The overall best is the max/min across non-pruned branches.
    }
    // what are the meaning of flag?
    //This tells future probes how reliable the score is (e.g., for upper bound, only use if it helps prune lows)
    //TT_UPPER means upper bound, i.e. we failed low, no move improved beyond alpha
    //TT_LOWER means lower bound, i.e. we failed high, position too good, the move is better than beta
    //TT_EXACT means alpha = beta = bestEval
    int flag = (bestEval <= orig_alpha) ? TT_UPPER : (bestEval >= orig_beta ? TT_LOWER : TT_EXACT);
    tt.store(board->zh->hash, board->zh->hash2, bestEval, depth, flag, best_src, best_dst, best_promo, ply_num);
    return bestEval;
}

std::pair<std::string, int> iterativeSearch(struct Board * board, int depth, int alpha, int beta, int original_color, struct NNUEContext *ctx) {
    int tt_score; int tt_src = SquareNone, tt_dst = SquareNone, tt_promo = PieceTypeNone;
    tt.probe(board->zh->hash, board->zh->hash2, depth, alpha, beta, tt_score, tt_src, tt_dst, tt_promo, PLY_NUM(board->fen));

    int best_score = -INT_MAX;
    std::string best_uci;
    generateMoves(board);
    auto movelist = getSortedMoves(board);
    if (tt_src != SquareNone) {
        for (auto it = movelist.begin(); it != movelist.end(); ++it) {
            if (it->src == tt_src && it->dst == tt_dst && it->promo == tt_promo) {
                std::iter_swap(it, movelist.begin()); //make the tt move first, why?
                break;
            }
        }
    }

    for (auto& sm : movelist) {
        struct Move move;
        init_move(&move, board, sm.src, sm.dst, sm.promo);
        make_move(&move);
        unsigned long long prevHash = board->zh->hash, 
                           prevHash2 = board->zh->hash2, 
                           prevEnPassant = board->zh->prevEnPassant, 
                           prevEnPassant2 = board->zh->prevEnPassant2, 
                           prevCastlingRights = board->zh->prevCastlingRights, 
                           prevCastlingRights2 = board->zh->prevCastlingRights2;        
        updateHash(board, &move);
        int score = alphaBetaMinimax(board, depth - 1, alpha, beta, original_color, ctx);
        undo_move(&move);
        board->zh->hash = prevHash;
        board->zh->hash2 = prevHash2; 
        board->zh->prevEnPassant = prevEnPassant;
        board->zh->prevEnPassant2 = prevEnPassant2;
        board->zh->prevCastlingRights = prevCastlingRights;
        board->zh->prevCastlingRights2 = prevCastlingRights2;

        if (score > best_score) {
            best_score = score;
            char buf[6] = {0};
            strcat(buf, squareName[sm.src]);
            strcat(buf, squareName[sm.dst]);
            if (sm.promo != PieceTypeNone) buf[4] = uciPromoLetter[sm.promo];
            best_uci = buf;
        }
    }
    return {best_uci, best_score};
}

std::pair<std::string, int> search(struct Board * board, int max_depth, struct NNUEContext * ctx) {
    if (max_depth <= 0) return {"", 0.0};

    int original_color = board->fen->sideToMove;
    std::string best_move;
    int best_score = 30.0;
    int alpha = -INT_MAX, beta = INT_MAX;
    tt.new_search();
    ply = PLY_NUM(board->fen);

    // Iterative deepening
    for (int depth = 1; depth <= max_depth; ++depth) {
        nodes = 0;  // Reset per iteration
        int asp_margin = 30; //10.0 * depth;
        int asp_alpha = best_score - asp_margin;
        int asp_beta = best_score + asp_margin;
        auto [move_d, score_d] = iterativeSearch(board, depth, asp_alpha, asp_beta, original_color, ctx);
        // Re-search full or wider if aspiration fail (less frequent with wide margin)
        if (score_d <= asp_alpha || score_d >= asp_beta) {
            printf("info string Aspiration fail at depth %d, re-searching full\n", depth);
            //alpha = -INT_MAX; beta = INT_MAX;
            alpha = best_score - 3 * asp_margin;
            beta = best_score + 3 * asp_margin;
            auto [move_full, score_full] = iterativeSearch(board, depth, alpha, beta, original_color, ctx);
            move_d = move_full;
            score_d = score_full;
        }        
        best_move = move_d;
        best_score = score_d;
        printf("info depth %d score cp %d nodes %lld pv %s\n", depth, best_score, nodes, best_move.c_str());
    }
    return {best_move, best_score};
}

std::string findBestMove(struct Board * board, int max_depth, struct NNUEContext * ctx) {
    if (max_depth <= 0) return "";
    auto [best_uci, _] = search(board, max_depth, ctx);
    return best_uci;
}

int main(int argc, char ** argv) {
    struct Board board;
    struct Fen fen;
    struct ZobristHash zh;
    struct Move move;
    struct NNUEContext ctx;
    char fenString[MAX_FEN_STRING_LEN] = "";
    if (argc == 1) strncpy(fenString, startPos, MAX_FEN_STRING_LEN);
    else if (argc >= 7) {
        for (int i = 1; i < 7; i++) {
            strcat(fenString, argv[i]);
            strcat(fenString, " ");
        }
    } 
    //tb_init(SYZYGY_PATH);
    //if (TB_LARGEST == 0) {
    //    fprintf(stderr, "info string error unable to initialize tablebase; no tablebase files found in %s\n", SYZYGY_PATH);
    //} else {
    //    fprintf(stdout, "info string successfully initialized tablebases in %s. Max number of pieces %d\n", SYZYGY_PATH, TB_LARGEST);
    //}
    init_magic_bitboards();
    if (strtofen(&fen, fenString)) {
        printf("test_nnue error: strtofen() failed; FEN %s\n", fenString);
        return 1;
    }
    if (fentoboard(&fen, &board)) {
        printf("test_nnue error: fentoboard() failed; FEN %s\n", fen.fenString);
        return 1;
    }
    zobristHash(&zh);
    board.zh = &zh;
    getHash(&zh, &board);  // Compute initial hash after board setup
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
    init_nnue_context(&ctx);
    if (board.isCheck) {
        printf("%s is checked\n", color[board.fen->sideToMove]);
    } else if (board.isMate) {
        printf("%s is mated\n", color[board.fen->sideToMove]);
    } else if (board.isStaleMate) {
        printf("stalemate, %s has no moves\n", color[board.fen->sideToMove]);
    } else {
        int depth = 10;
        std::string bestmove = findBestMove(&board, depth, &ctx);
        printf("bestmove %s\n", bestmove.c_str());
    }
    free_nnue_context(&ctx);
    cleanup_nnue();
    cleanup_magic_bitboards();
    return 0;
}