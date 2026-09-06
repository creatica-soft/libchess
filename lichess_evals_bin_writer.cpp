// c++ -std=c++20 -Wno-writable-strings -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess lichess_evals_bin_writer.cpp -o lichess_evals_bin_writer
//lichess_db_eval.jsonl contains at least 3,003,377 illegal positions out of 288,977,589 
//such as rnbqk1nr/1pp2ppp/pbnp4/3Pp3/B3P3/2P2N2/PP3PPP/RNBQKBNR b KQkq - 0 1
//so we need to take care of at least skipping positions with more than 32 pieces and we should check for other things too!
//bb1nqrkr/p1pp1ppp/1p1n4/3Np3/4P3/1P6/P1PP1PPP/BBN1QRKR b KQkq - 0 1
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <unordered_set>
#include "nnue/bitboard.h"
#include "json.hpp"
#include "libchess.h"
// PV count field width and the cap on PVs written. THESE TWO MUST AGREE: the count is
// stored in PV_COUNT_BITS bits, so writing more than (1 << PV_COUNT_BITS) PVs silently
// truncates the count while still emitting every entry, and the reader then consumes
// the wrong number and desyncs the rest of the file. Removing the cap without widening
// the field is exactly what corrupted lichess_db_pvs_eval*.bin -- measured over 200,000
// records, 12.62% carried more than 4 PVs. Distribution: 1 PV 61.7%, 2 8.1%, 3 15.0%,
// 4 2.7%, 5 12.5%, 7-18 about 0.1% combined. A cap of 8 loses 0.08% of records.
static constexpr int PV_COUNT_BITS = 4;
static constexpr int MAX_PVS       = 1 << PV_COUNT_BITS;   // 16

// --- Binary Format Spec ---
// [5 bits] Num Pieces; to allow 32 pieces, we subtract 1 on encoding and add 1 on decoding
// [10 bits] Per Piece: Square(6) | Color(1) | Type(3) x number of pieces (max 32)!
// --- followed by 25 bits ---
// [1 bit] Side to Move (0=White, 1=Black)
// [4 bits] Castling (KQkq)
// [4 bits] En Passant File (0-7, 8=None)
// [16 bits] Eval CP for PV1 (Two's complement int16)
// [PV_COUNT_BITS bits] num_pvs - 1 (0 = 1 PV ... 7 = 8 PVs)
// For each PV:
//   [6 bits] from_sq
//   [6 bits] to_sq  
//   [2 bits] promo (0=Q,1=N,2=B,3=R) - could skip this and use q promo as default
//   [16 bits] cp score  <- skip for PV1 since already written above
// [Padding] Zero bits to reach byte boundary
const bool unique_positions = false;
uint64_t skipped = 0, duplicate = 0;
constexpr size_t NUM_PARTITIONS = 256;
constexpr size_t BITS = 8;
std::array<std::unordered_set<uint64_t>, NUM_PARTITIONS> positions;

const int mate_score = 20000;
Zobrist z = {};

// Function to get partition index from hash (top BITS bits)
//inline constexpr size_t get_partition(uint64_t hash) {
//    return (hash >> (64 - BITS)) & (NUM_PARTITIONS - 1);  // Bits 63,62,61
//}

// To insert (only if not exists, for dedup)
void insert(const uint64_t hash) {
    //size_t part = get_partition(hash);
    auto& set = positions[(hash >> (64 - BITS)) & (NUM_PARTITIONS - 1)];
    set.insert(hash);
}

// To check existence
bool contains(const uint64_t hash) {
    //size_t part = get_partition(hash);
    return positions[(hash >> (64 - BITS)) & (NUM_PARTITIONS - 1)].contains(hash);  // Or find() != end()
}

struct ParsedFen {
    struct Piece {
        int square; // 0-63
        int color; // 0 or 1
        int type; // 1-6
    };
    std::vector<Piece> pieces;
    int side_to_move;
    int castling_rights; // Bitmask: qkQK (4 bits)
    int ep_file; // 0-7, 8 if none
};

class BitStream {
private:
    std::ofstream& out;
    uint64_t accumulator; //max 64 bits! The position is only 31 bits + 1 bit padding
    int bits_in_buffer;
public:
    BitStream(std::ofstream& o) : out(o), accumulator(0), bits_in_buffer(0) {}
    ~BitStream() {
        align();
    }
    // Write n bits (LSB of val is written first to the bitstream)
    void write(uint32_t val, int n_bits) {
        uint64_t masked_val = static_cast<uint64_t>(val) & ((1ULL << n_bits) - 1);
        accumulator |= (masked_val << bits_in_buffer);
        bits_in_buffer += n_bits;
        while (bits_in_buffer >= 8) {
            out.put(static_cast<char>(accumulator & 0xFF));
            accumulator >>= 8;
            bits_in_buffer -= 8;
        }
    }
    void align() {
        if (bits_in_buffer > 0) {
            out.put(static_cast<char>(accumulator & 0xFF));
            accumulator = 0;
            bits_in_buffer = 0;
        }
    }
};

bool is_legal(const ParsedFen& p_fen) {
  if (p_fen.pieces.size() > 32) {
    //std::cerr << p_fen.pieces.size() << " chess pieces\n";
    return false;
  }
  Board board = {};
  board.sideToMove = (Color)p_fen.side_to_move;
  board.enPassant = (File)p_fen.ep_file;
  board.castlingRights = p_fen.castling_rights;
  
  int white_pawns = 0, black_pawns = 0, white_knights = 0, black_knights = 0, white_bishops = 0, black_bishops = 0, white_rooks = 0, black_rooks = 0, white_queens = 0, black_queens = 0, white_king = 0, black_king = 0;
  int white_king_sq = SquareNone, black_king_sq = SquareNone, rank = RankNone;
  for (const auto& p : p_fen.pieces) {
    switch (p.type) {
    case Pawn:
      rank = p.square >> 3;
      if (rank == 0 || rank == 7) {
        const std::string color = p.color == ColorWhite ? "white" : "black";
        //std::cerr << color << " pawn on first or last rank\n";
        return false; //pawn on first or last rank
      }
      if (p.color == ColorWhite) white_pawns++;
      else black_pawns++;
      break;
    case Knight:
      if (p.color == ColorWhite) white_knights++;
      else black_knights++;
      break;
    case Bishop:
      if (p.color == ColorWhite) white_bishops++;
      else black_bishops++;
      break;
    case Rook:
      if (p.color == ColorWhite) white_rooks++;
      else black_rooks++;
      break;
    case Queen: 
      if (p.color == ColorWhite) white_queens++;
      else black_queens++;
      break;
    case King:
      if (p.color == ColorWhite) {
        white_king++;
        white_king_sq = p.square;
      }
      else {
        black_king++;
        black_king_sq = p.square;
      }
      break;
    }          
    board.piecesOnSquares[p.square] = static_cast<Piece>((p.color << 3) | p.type);
    board.pieceTypes[p.type - 1] |= (1ULL << p.square);
    board.side[p.color] |= (1ULL << p.square);
  }
  if (p_fen.castling_rights & 1) board.castlingRooks |= msBit(board.side[ColorWhite] & board.pieceTypes[Rook - 1]);
  if (p_fen.castling_rights & 2) board.castlingRooks |= lsBit(board.side[ColorWhite] & board.pieceTypes[Rook - 1]);
  if (p_fen.castling_rights & 4) board.castlingRooks |= msBit(board.side[ColorBlack] & board.pieceTypes[Rook - 1]);
  if (p_fen.castling_rights & 8) board.castlingRooks |= lsBit(board.side[ColorBlack] & board.pieceTypes[Rook - 1]);

  if (white_king == 0) {
    //std::cerr << "missing white king\n";
    return false;
  }
  if (black_king == 0) {
    //std::cerr << "missing black king\n";
    return false;
  }
  if (white_king > 1) {
    //std::cerr << white_king << " white kings\n";
    return false;
  } 
  if (black_king > 1) {
    //std::cerr << black_king << " black kings\n";
    return false;
  }
  if (white_pawns > 8) {
    //std::cerr << white_pawns << " white pawns\n";
    return false;    
  }
  if (black_pawns > 8) {
    //std::cerr << black_pawns << " black pawns\n";
    return false;    
  }
  int ex_pieces = 0;
  if (white_knights > 2) ex_pieces += (white_knights - 2);
  if (white_bishops > 2) ex_pieces += (white_bishops - 2);
  if (white_rooks > 2) ex_pieces += (white_rooks - 2);
  if (white_queens > 1) ex_pieces += (white_queens - 1);
  if (white_pawns + ex_pieces > 8) {
    //std::cerr << "too many white promo pieces for the number of white pawns\n";
    return false;          
  }
  ex_pieces = 0;
  if (black_knights > 2) ex_pieces += (black_knights - 2);
  if (black_bishops > 2) ex_pieces += (black_bishops - 2);
  if (black_rooks > 2) ex_pieces += (black_rooks - 2);
  if (black_queens > 1) ex_pieces += (black_queens - 1);
  if (black_pawns + ex_pieces > 8) {
    //std::cerr << "too many black promo pieces for the number of black pawns\n";
    return false;          
  }
  int diff = std::abs(white_king_sq - black_king_sq);
  int wk_rank = white_king_sq >> 3;
  int bk_rank = black_king_sq >> 3;
  int rank_diff = std::abs(wk_rank - bk_rank);
  if ((diff == 1 && wk_rank == bk_rank) || (diff >= 7 && diff <= 9 && rank_diff == 1)) {
    //std::cerr << "king attacks opponent's king\n";
    return false;
  }
  //check if en passant is legal meaning if there are opposite color adjacent pawns on rank 5 if white to move or rank 4 if black to move
  //bool ep_legal = false;
  if (p_fen.ep_file < FileNone) { //en passant is set
    int ep_rank = (p_fen.side_to_move == ColorWhite) ? Rank6 : Rank3;
    int ep_square = (ep_rank << 3) | p_fen.ep_file;
    int victim_square = (p_fen.side_to_move == ColorWhite) ? ep_square - 8 : ep_square + 8;
    int expected_victim = (p_fen.side_to_move == ColorWhite) ? BlackPawn : WhitePawn;

    if (board.piecesOnSquares[victim_square] != expected_victim) {
        //std::cerr << "Illegal EP: No victim pawn behind the en-passant square.\n";
        return false;
    }
  } //end of en passant check
  
  // White King Side (K) -> Requires White Rook on h1 and White King on e1
  if ((p_fen.castling_rights & 0b1) && 
     (board.piecesOnSquares[SquareH1] != WhiteRook || board.piecesOnSquares[SquareE1] != WhiteKing)) {
     //std::cerr << "White King Side (K) castling requires White Rook on h1 and White King on e1\n";
     return false;
  }

  // White Queen Side (Q) -> Requires White Rook on a1 and White King on e1
  if ((p_fen.castling_rights & 0b10) && 
     (board.piecesOnSquares[SquareA1] != WhiteRook || board.piecesOnSquares[SquareE1] != WhiteKing)) {
     //std::cerr << "White Queen Side (Q) castling requires White Rook on a1 and White King on e1\n"; 
     return false;
  }

  // Black King Side (k) -> Requires Black Rook on h8 and Black King on e8
  if ((p_fen.castling_rights & 0b100) && 
     (board.piecesOnSquares[SquareH8] != BlackRook || board.piecesOnSquares[SquareE8] != BlackKing)) {
     //std::cerr << "Black King Side (k) castling requires Black Rook on h8 and Black King on e8\n";
     return false;
  }

  // Black Queen Side (q) -> Requires Black Rook on a8 and Black King on e8
  if ((p_fen.castling_rights & 0b1000) && 
     (board.piecesOnSquares[SquareA8] != BlackRook || board.piecesOnSquares[SquareE8] != BlackKing)) { 
     //std::cerr << "Black Queen Side (q) castling requires Black Rook on a8 and Black King on e8\n";
     return false;
  }
  //check if side to move gives check
	if (board.sideToMove == ColorWhite) {
	  board.sideToMove = ColorBlack;
	  uint64_t whiteAttacks = getAttackedSquaresOnly(board); //returns squares attacked by white
	  if (whiteAttacks & board.pieceTypes[King - 1] & board.side[ColorBlack]) {
	    //std::cerr << "white is to move but white give check!\n";
	    return false;
	  }
	  board.sideToMove = ColorWhite;
	} else {
	  board.sideToMove = ColorWhite;
	  uint64_t blackAttacks = getAttackedSquaresOnly(board); //returns squares attacked by black
	  if (blackAttacks & board.pieceTypes[King - 1] & board.side[ColorWhite]) {
	    //std::cerr << "black is to move but black give check!\n";
	    return false;
	  }
	  board.sideToMove = ColorBlack;
	}
  return true;
}

ParsedFen parse_fen_string(const std::string& fen) {
    ParsedFen res;
    res.side_to_move = ColorWhite;
    res.castling_rights = 0;
    res.ep_file = FileNone; // Default to 8 (None)
    size_t space1 = fen.find(' ');
    std::string board_part = fen.substr(0, space1);
   
    // 1. Parse Pieces
    int rank = 7;
    int file = 0;
   
    for (char c : board_part) {
        if (c == '/') {
            rank--;
            file = 0;
        } else if (isdigit(c)) {
            file += (c - '0');
        } else {
            int square = rank * 8 + file;
            int color = isupper(c) ? ColorWhite : ColorBlack;
            int type = PieceTypeNone;
            char lower = tolower(c);
            switch(lower) {
                case 'p': type = Pawn; break;
                case 'n': type = Knight; break;
                case 'b': type = Bishop; break;
                case 'r': type = Rook; break;
                case 'q': type = Queen; break;
                case 'k': type = King; break;
            }
            if (type == PieceTypeNone) {
              std::cerr << "parse_fen_string() error: type is PieceTypeNone, fen " << fen << "\n";
              exit(-1);
            }
            res.pieces.push_back({square, color, type});
            file++;
        }
    }
    if (space1 == std::string::npos) return res;
    // 2. Parse Side to Move
    size_t space2 = fen.find(' ', space1 + 1);
    std::string color_part = fen.substr(space1 + 1, space2 - space1 - 1);
    if (color_part == "b") res.side_to_move = ColorBlack;
    if (space2 == std::string::npos) return res;
    // 3. Parse Castling
    size_t space3 = fen.find(' ', space2 + 1);
    std::string castle_part = fen.substr(space2 + 1, space3 - space2 - 1);
    if (castle_part != "-") {
        for (char c : castle_part) {
            if (c == 'K') res.castling_rights |= (1 << 0);
            if (c == 'Q') res.castling_rights |= (1 << 1);
            if (c == 'k') res.castling_rights |= (1 << 2);
            if (c == 'q') res.castling_rights |= (1 << 3);
        }
    }
    if (space3 == std::string::npos) return res;
    // 4. Parse En Passant
    size_t space4 = fen.find(' ', space3 + 1);
    std::string ep_part = fen.substr(space3 + 1, space4 - space3 - 1);
    if (ep_part != "-") {
        if (ep_part.length() >= 1) {
            char file_char = ep_part[0];
            if (file_char >= 'a' && file_char <= 'h') {
                res.ep_file = file_char - 'a';
            }
        }
    }
    return res;
}

bool parse_uci_move(const std::string& line, int& from_sq, int& to_sq, int& promo) {
    if (line.size() < 4) return false;
    const std::string& move = line.substr(0, line.find(' ')); // first move only
    if (move.size() < 4) return false;
    int from_file = move[0] - 'a';
    int from_rank = move[1] - '1';
    int to_file   = move[2] - 'a';
    int to_rank   = move[3] - '1';
    if (from_file < 0 || from_file > 7 || from_rank < 0 || from_rank > 7) return false;
    if (to_file   < 0 || to_file   > 7 || to_rank   < 0 || to_rank   > 7) return false;
    from_sq = from_rank * 8 + from_file;
    to_sq   = to_rank   * 8 + to_file;
    promo = 0;
    if (move.size() >= 5) {
        switch (move[4]) {
            case 'n': promo = 1; break;
            case 'b': promo = 2; break;
            case 'r': promo = 3; break;
            case 'q': promo = 0; break; 
        }
    }
    return true;
}

uint64_t getHash(ParsedFen& p_fen) {	
	uint64_t hash = 0;
	std::unordered_set<int> occupied_squares(32);
	for (auto& piece : p_fen.pieces) {
	  occupied_squares.insert(piece.square);
	  if (piece.type == Pawn) hash ^= z.pawns[piece.color][piece.square - 8];
	  else hash ^= z.nonPawns[piece.color][piece.type - 2][piece.square];
	}
	for (int sn = SquareA1; sn <= SquareH8; sn++) {
  	if (!occupied_squares.contains(sn)) hash ^= z.emptySquares[sn];
  }
	hash ^= z.castling[p_fen.castling_rights];
	if (p_fen.ep_file != FileNone) hash ^= z.enPassant[p_fen.ep_file];
	if (p_fen.side_to_move == ColorBlack) hash ^= z.blackMove;
	return hash;
}

class my_sax : public nlohmann::json_sax<nlohmann::json> {
private:
    BitStream& stream;
    int object_level = 0;
    std::string fen;
    int max_depth;
   
    //struct PV { int cp; bool is_mate; std::string line; };
    struct PV { 
        int cp; 
        bool is_mate; 
        std::string line; 
        int from_sq = -1;
        int to_sq = -1;
        int promo = 0; // 0=Q,1=N,2=B,3=R
    };
    struct Eval { std::vector<PV> pvs; std::int64_t knodes; int depth; };
   
    Eval best_eval;
    bool in_evals = false;
    Eval current_eval;
    bool in_pvs = false;
    PV current_pv;
    std::string current_key, pv_key;
    // Helper to handle numbers regardless of sign
    void handle_metric(std::int64_t val) {
        if (in_pvs) {
            if (pv_key == "cp") {
                current_pv.cp = static_cast<int>(val);
                current_pv.is_mate = false;
            } else if (pv_key == "mate") {
                int mate_in = static_cast<int>(val);
                // Calculate mate score
                if (mate_in > 0) {
                    current_pv.cp = mate_score - mate_in;
                } else {
                    current_pv.cp = -mate_score - mate_in;
                }
                current_pv.is_mate = true;
            }
        } else if (in_evals) {
            if (current_key == "knodes") {
                current_eval.knodes = val;
            } else if (current_key == "depth") {
                current_eval.depth = static_cast<int>(val);
            }
        }
    }
public:
    my_sax(BitStream& s) : stream(s) {}
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
   
    // Triggered for negative numbers (e.g., -15)
    bool number_integer(number_integer_t val) override {
        handle_metric(val);
        return true;
    }
   
    // Triggered for positive numbers (e.g., 48) - THIS WAS MISSING
    bool number_unsigned(number_unsigned_t val) override {
        handle_metric(static_cast<std::int64_t>(val));
        return true;
    }
   
    bool number_float(number_float_t, const string_t&) override { return true; }
   
    bool string(string_t& val) override {
        if (in_pvs) { if (pv_key == "line") current_pv.line = val; }
        else if (!in_evals) { if (current_key == "fen") fen = val; }
        return true;
    }
   
    bool binary(binary_t&) override { return true; }
   
    bool start_object(std::size_t) override {
        ++object_level;
        if (object_level == 1) { fen = ""; max_depth = -1; best_eval = {{}, 0, -1}; }
        else if (in_evals && !in_pvs) { current_eval = {{}, 0, 0}; }
        else if (in_pvs) { current_pv = {0, false, ""}; }
        return true;
    }
   
    bool key(string_t& val) override {
        if (in_pvs) pv_key = val; else current_key = val;
        return true;
    }
   
    bool end_object() override {
        if (in_pvs) current_eval.pvs.push_back(current_pv);
        else if (in_evals && object_level > 1) {
            // Only update best_eval if this depth is strictly greater
            if (current_eval.depth > max_depth) {
                max_depth = current_eval.depth;
                best_eval = current_eval;
            }
        }
       
        /*if (object_level == 1) {
            // Write to Binary Stream
            if (max_depth != -1 && !best_eval.pvs.empty()) {
                ParsedFen p_fen = parse_fen_string(fen);
                if (!is_legal(p_fen)) { //illegal position 
                  skipped++;
                  //std::cerr << "illegal position " << skipped << ": " << fen << "\n";
                  --object_level;
                  return true;
                }
                if (unique_positions) {
                  uint64_t hash = getHash(p_fen);
                  if (contains(hash)) { //duplicate position
                    duplicate++;
                    std::cerr << "duplicate position " << duplicate << ": " << fen << "\n";
                    --object_level;
                    return true;
                  } else insert(hash);
                }
                
                // [6 bits] Num Pieces
                stream.write(p_fen.pieces.size() - 1, 5);
               
                // [10 bits per piece]
                for (const auto& p : p_fen.pieces) {
                    stream.write(p.square, 6);
                    stream.write(p.color, 1);
                    stream.write(p.type, 3);
                }
               
                // Global State
                stream.write(p_fen.side_to_move, 1);
                stream.write(p_fen.castling_rights, 4);
                stream.write(p_fen.ep_file, 4);
               
                // [16 bits] Eval CP
                stream.write(static_cast<uint16_t>(static_cast<int16_t>(best_eval.pvs[0].cp)), 16);
               
                stream.align();
            }
        }*/
        
        if (object_level == 1) {
            if (max_depth != -1 && !best_eval.pvs.empty()) {
                ParsedFen p_fen = parse_fen_string(fen);
                if (!is_legal(p_fen)) { skipped++; --object_level; return true; }
                if (unique_positions) {
                    uint64_t hash = getHash(p_fen);
                    if (contains(hash)) { duplicate++; --object_level; return true; }
                    else insert(hash);
                }
        
                // Parse moves for all PVs
                std::vector<PV> pvs = best_eval.pvs;
                // Cap at 3, filter out PVs with unparseable moves
                std::vector<PV> valid_pvs;
                for (auto& pv : pvs) {
                    int from_sq, to_sq, promo;
                    if (parse_uci_move(pv.line, from_sq, to_sq, promo)) {
                        pv.from_sq = from_sq;
                        pv.to_sq   = to_sq;
                        pv.promo   = promo;
                        valid_pvs.push_back(pv);
                    }
                    if (valid_pvs.size() == MAX_PVS) break;   // must match PV_COUNT_BITS
                }
                if (valid_pvs.empty()) { skipped++; --object_level; return true; }
        
                // Write pieces
                stream.write(p_fen.pieces.size() - 1, 5);
                for (const auto& p : p_fen.pieces) {
                    stream.write(p.square, 6);
                    stream.write(p.color, 1);
                    stream.write(p.type, 3);
                }
        
                // Write board state
                stream.write(p_fen.side_to_move, 1);
                stream.write(p_fen.castling_rights, 4);
                stream.write(p_fen.ep_file, 4);
        
                // Write PV1 cp (backward compatible)
                stream.write(static_cast<uint16_t>(static_cast<int16_t>(valid_pvs[0].cp)), 16);
        
                // Write num_pvs - 1 (2 bits: 0,1,2 for 1,2,3 PVs)
                stream.write(valid_pvs.size() - 1, PV_COUNT_BITS);
        
                // Write each PV's move + cp (cp skipped for PV1 since already written)
                for (size_t i = 0; i < valid_pvs.size(); i++) {
                    stream.write(valid_pvs[i].from_sq, 6);
                    stream.write(valid_pvs[i].to_sq, 6);
                    stream.write(valid_pvs[i].promo, 2);
                    if (i > 0) {
                        stream.write(static_cast<uint16_t>(static_cast<int16_t>(valid_pvs[i].cp)), 16);
                    }
                }
        
                stream.align();
            }
        }        
        
        --object_level;
        return true;
    }
   
    bool start_array(std::size_t) override {
        if (current_key == "evals") in_evals = true;
        else if (current_key == "pvs") { in_pvs = true; current_eval.pvs.clear(); }
        return true;
    }
   
    bool end_array() override {
        if (in_pvs) in_pvs = false;
        else if (in_evals) in_evals = false;
        return true;
    }
   
    bool parse_error(std::size_t, const std::string&, const nlohmann::json::exception&) override { return false; }
};
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.json output.bin" << std::endl;
        return 1;
    }
  
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "Cannot open input file: " << argv[1] << std::endl;
        return 1;
    }
  
    std::string base(argv[2]);
    size_t dot_pos = base.rfind('.');
    std::string prefix, ext;
    if (dot_pos != std::string::npos) {
        prefix = base.substr(0, dot_pos);
        ext = base.substr(dot_pos);
    } else {
        prefix = base;
        ext = "";
    }
  
    int part = 0;
    std::ofstream out;
    std::string filename = base; // First file uses the provided output name
    out.open(filename, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Cannot open output file: " << filename << std::endl;
        return 1;
    }
  
    std::optional<BitStream> bitStream;
    bitStream.emplace(out);
    if (unique_positions) {
      for (auto& set : positions) {
          set.reserve(290000000 / NUM_PARTITIONS + 100000);  // Add buffer for skew
      }
    }
    zobristHash(z);
    Stockfish::Bitboards::init();
    // Read line by line
    std::string line;
    long long line_count = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
      
        // Reset SAX handler for each line, but keep the BitStream
        my_sax handler(*bitStream);
      
        // Parse the line
        bool parse_success = nlohmann::json::sax_parse(line, &handler);
      
        if (!parse_success) {
            std::cerr << "JSON parse error on line " << line_count + 1 << std::endl;
            // Optional: return 1; or continue to next line
        }
        line_count++;
      
        if (line_count % 100000 == 0) {
            std::cout << "Processed " << line_count << " positions..." << std::endl;
        }
      
        if (line_count % 10000000 == 0) {
            bitStream->align();
            bitStream.reset();
            out.close();
            std::cout << "Finished processing " << line_count << " lines." << std::endl;
            //return 0;
            part++;
            filename = prefix + "_" + std::to_string(part) + ext; // e.g., output_1.bin
            out.open(filename, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "Cannot open output file: " << filename << std::endl;
                return 1;
            }
            bitStream.emplace(out);
        }
    }
  
    std::cout << "Finished processing " << line_count << " lines." << std::endl;
    std::cout << "Illegal positions: " << skipped << "\n";
    if (unique_positions) std::cout << "Duplicate positions: " << duplicate << "\n";
    return 0;
}