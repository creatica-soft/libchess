/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>

#include "nnue/network.h"
#include "nnue/nnue_misc.h"
//#include "position.h"
#include "types.h"
//#include "uci.h"
#include "nnue/nnue_accumulator.h"
#include "board.h"

namespace Stockfish {

// Returns a static, purely materialistic evaluation of the position from
// the point of view of the side to move. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
/*int Eval::simple_eval(const Position& pos) {
    Color c = pos.side_to_move();
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c)) + pos.non_pawn_material(c)
         - pos.non_pawn_material(~c);
}*/

int Eval::simple_eval(const Board& board) {
    int c = board.sideToMove;
    int pawns = popcount(board.pieceTypes[PAWN - 1] & board.side[c]);
    int opp_pawns = popcount(board.pieceTypes[PAWN - 1] & board.side[c ^ 1]);
    //Was `side[c] ^ pieceTypes[PAWN-1]`. XOR only equals "clear the pawns" when the pawn
    //set is a SUBSET of the operand -- true at line 137 below, where the operand is all
    //occupied squares, but false here, where it is one side's pieces. The XOR therefore
    //pulled in the OPPONENT's pawns, and the algebra made the pawn terms cancel outright:
    //  np      = my_non_pawns  + opp_pawns * PawnValue
    //  opp_np  = opp_non_pawns + my_pawns  * PawnValue
    //  result  = PawnValue*(pawns - opp_pawns) + (np - opp_np) = my_non_pawns - opp_non_pawns
    //so simple_eval ignored pawns entirely, and use_smallnet() routed on non-pawn material
    //alone.
    unsigned long long non_pawns = board.side[c] & ~board.pieceTypes[PAWN - 1];
    int np = 0;
    while (non_pawns) {
      int sq = pop_lsb(non_pawns);
      np += PieceValue[board.piecesOnSquares[sq]];
    }
    unsigned long long opp_non_pawns = board.side[c ^ 1] & ~board.pieceTypes[PAWN - 1];
    int opp_np = 0;
    while (opp_non_pawns) {
      int sq = pop_lsb(opp_non_pawns);
      opp_np += PieceValue[board.piecesOnSquares[sq]];
    }
    return PawnValue * (pawns - opp_pawns) + (np - opp_np);
}

//bool Eval::use_smallnet(const Position& pos) { return std::abs(simple_eval(pos)) > 962; }
bool Eval::use_smallnet(const Board& board) { return std::abs(simple_eval(board)) > 962; }

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
/*Value Eval::evaluate(const Eval::NNUE::Networks&    networks,
                     const Position&                pos,
                     Eval::NNUE::AccumulatorStack&  accumulators,
                     Eval::NNUE::AccumulatorCaches& caches,
                     int                            optimism) {

    assert(!pos.checkers());

    bool smallNet           = use_smallnet(pos);
    auto [psqt, positional] = smallNet ? networks.small.evaluate(pos, accumulators, caches.small)
                                       : networks.big.evaluate(pos, accumulators, caches.big);

    Value nnue = (125 * psqt + 131 * positional) / 128;

    // Re-evaluate the position when higher eval accuracy is worth the time spent
    if (smallNet && (std::abs(nnue) < 277))
    {
        std::tie(psqt, positional) = networks.big.evaluate(pos, accumulators, caches.big);
        nnue                       = (125 * psqt + 131 * positional) / 128;
        smallNet                   = false;
    }

    // Blend optimism and eval with nnue complexity
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * nnueComplexity / 476;
    nnue -= nnue * nnueComplexity / 18236;

    int material = 534 * pos.count<PAWN>() + pos.non_pawn_material();
    int v        = (nnue * (77871 + material) + optimism * (7191 + material)) / 77871;

    // Damp down the evaluation linearly when shuffling
    v -= v * pos.rule50_count() / 199;

    // Guarantee evaluation does not hit the tablebase range
    v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}*/

Value Eval::evaluate(const Eval::NNUE::Networks& networks, const Board& board, Eval::NNUE::AccumulatorStack&  accumulators, Eval::NNUE::AccumulatorCaches& caches, int optimism) {

    //assert(!pos.checkers());
    assert(!board.isCheck);

    bool smallNet = use_smallnet(board);
    auto [psqt, positional] = smallNet ? networks.small.evaluate(board, accumulators, caches.small) : networks.big.evaluate(board, accumulators, caches.big);

    Value nnue = (125 * psqt + 131 * positional) / 128; //uses different weigths compared to trace where v = psqt + positional

    // Re-evaluate the position when higher eval accuracy is worth the time spent
    if (smallNet && (std::abs(nnue) < 277)) {
        std::tie(psqt, positional) = networks.big.evaluate(board, accumulators, caches.big);
        nnue = (125 * psqt + 131 * positional) / 128;
        smallNet = false;
    }

    // Blend optimism and eval with nnue complexity
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * nnueComplexity / 476;
    nnue -= nnue * nnueComplexity / 18236; //I guess some kind of conservative measure proportional to complexity

    //int material = 534 * pos.count<PAWN>() + pos.non_pawn_material();
    int pawns = popcount(board.pieceTypes[PAWN - 1]);
    unsigned long long non_pawns = ((board.side[WHITE] | board.side[BLACK])) ^ board.pieceTypes[PAWN - 1];
    int np = 0;
    while (non_pawns) {
      int sq = pop_lsb(non_pawns);
      np += PieceValue[board.piecesOnSquares[sq]];
    }
    int material = 534 * pawns + np;
    //printf("Eval::evaluate() debug: material %d\n", material);
    int v = (nnue * (77871 + material) + optimism * (7191 + material)) / 77871; //adding material to nnue eval

    // Damp down the evaluation linearly when shuffling
    //v -= v * pos.rule50_count() / 199;
    v -= v * board.halfmoveClock / 199;
    
    // Guarantee evaluation does not hit the tablebase range
    v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}

Eval::WinRateParams win_rate_params(const Board& board) {
    int pawns = popcount(board.pieceTypes[PAWN - 1]); 
    int knights = popcount(board.pieceTypes[KNIGHT - 1]);
    int bishops = popcount(board.pieceTypes[BISHOP - 1]);
    int rooks = popcount(board.pieceTypes[ROOK - 1]);
    int queens = popcount(board.pieceTypes[QUEEN - 1]);
    int material = pawns + 3 * knights + 3 * bishops + 5 * rooks + 9 * queens;
    //printf("win_rate_params() debug: pawns %d, knights %d, bishops %d, rooks %d, queens %d, total material %d\n", pawns, knights, bishops, rooks, queens, material);

    // The fitted model only uses data for material counts in [17, 78], and is anchored at count 58.
    double m = std::clamp(material, 17, 78) / 58.0;
    //printf("win_rate_params() debug: clamped material %f\n", m);

    // Return a = p_a(material) and b = p_b(material), see github.com/official-stockfish/WDL_model
    constexpr double as[] = {-13.50030198, 40.92780883, -36.82753545, 386.83004070};
    constexpr double bs[] = {96.53354896, -165.79058388, 90.89679019, 49.29561889};

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];
    //printf("win_rate_params() debug: a %f, b %f\n", a, b);

    return {a, b};
}

// The win rate model is 1 / (1 + exp((a - eval) / b)), where a = p_a(material) and b = p_b(material).
// It fits the LTC fishtest statistics rather accurately.
/*int win_rate_model(Value v, const Position& pos) {

    auto [a, b] = win_rate_params(pos);

    // Return the win rate in per mille units, rounded to the nearest integer.
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}*/
int win_rate_model(Value v, const Board& board) {

    auto [a, b] = win_rate_params(board);

    // Return the win rate in per mille units, rounded to the nearest integer.
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}
// Turns a Value to an integer centipawn number,
// without treatment of mate and similar special scores.
/*Value to_cp(Value v, const Position& pos) {

    // In general, the score can be defined via the WDL as
    // (log(1/L - 1) - log(1/W - 1)) / (log(1/L - 1) + log(1/W - 1)).
    // Based on our win_rate_model, this simply yields v / a.

    auto [a, b] = win_rate_params(pos);

    return std::round(100 * int(v) / a);
}*/

Value Eval::to_cp(Value v, const Board& board) {
    auto [a, b] = win_rate_params(board);
    return std::round(100 * int(v) / a);
}


// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
/*std::string Eval::trace(Position& pos, const Eval::NNUE::Networks& networks) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    auto accumulators = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches       = std::make_unique<Eval::NNUE::AccumulatorCaches>(networks);

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
    ss << '\n' << NNUE::trace(pos, networks, *caches) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    auto [psqt, positional] = networks.big.evaluate(pos, *accumulators, caches->big);
    Value v                 = psqt + positional;
    v                       = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    v = evaluate(networks, pos, *accumulators, *caches, VALUE_ZERO);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    return ss.str();
}*/

std::string Eval::trace(Board& board, const Eval::NNUE::Networks& networks) {

    if (board.isCheck)
        return "Final evaluation: none (in check)";

    auto accumulators = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches       = std::make_unique<Eval::NNUE::AccumulatorCaches>(networks);

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
    ss << '\n' << NNUE::trace(board, networks, *caches) << '\n'; //see nnue_misc.cpp

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    auto [psqt, positional] = networks.big.evaluate(board, *accumulators, caches->big);
    //printf("Eval::trace() debug: network.big.evaluate() return psqt %d and positional %d\n", psqt, positional);
    Value v                 = psqt + positional;
    v                       = static_cast<Color>(board.sideToMove) == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * to_cp(v, board) << " (white side)\n";

    v = evaluate(networks, board, *accumulators, *caches, VALUE_ZERO); //more complex eval function that uses the above nnue eval as a starting point
    v = static_cast<Color>(board.sideToMove) == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * to_cp(v, board) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    return ss.str();
}

}  // namespace Stockfish
