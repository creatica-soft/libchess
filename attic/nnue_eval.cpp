//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o nnue_eval nnue_eval.cpp

#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"

#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <iomanip>
#include "libchess.h"

struct NNUEContext {
    Stockfish::StateInfo * state;
    Stockfish::Position * pos;
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};

extern "C" {
  void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
  void cleanup_nnue();
  void init_nnue_context(NNUEContext * ctx);
  void free_nnue_context(NNUEContext * ctx);
  double evaluate_nnue(Board * chess_board, const int move, NNUEContext * ctx);
}

/*struct CompressedPosition {
    uint8_t piecesOnSquares[64];
    int side_to_move;    // 0=White, 1=Black
    int castling_rights; // Bitmask
    int ep_file;         // 0-7, 8=None
    int eval_cp;         // Centipawns
};*/

class BitReader {
private:
    std::ifstream& in;
    uint64_t accumulator;
    int bits_in_buffer;

public:
    BitReader(std::ifstream& i) : in(i), accumulator(0), bits_in_buffer(0) {}

    // Read n bits
    uint32_t read(int n_bits) {
        while (bits_in_buffer < n_bits) {
            // We need more bits, grab a byte from the file
            if (in.peek() == EOF) return 0; // Should handle gracefully
            
            char c;
            in.get(c);
            uint8_t byte = static_cast<uint8_t>(c);
            
            accumulator |= (static_cast<uint64_t>(byte) << bits_in_buffer);
            bits_in_buffer += 8;
        }

        uint32_t val = accumulator & ((1ULL << n_bits) - 1);
        accumulator >>= n_bits;
        bits_in_buffer -= n_bits;
        return val;
    }

    // Align to the next byte boundary (skip padding bits)
    void align() {
        accumulator = 0;
        bits_in_buffer = 0;
    }
    
    bool eof() {
        return in.peek() == EOF && bits_in_buffer == 0;
    }
};

bool read_next_position(BitReader& reader, Board * board, int * eval_cp) {
    if (reader.eof()) return false;

    memset(board->piecesOnSquares, PieceNameNone, sizeof board->piecesOnSquares);
    memset(board->pieceTypes, 0, sizeof board->pieceTypes);
    memset(board->side, 0, sizeof board->side);
    
    // 2. Read Number of Pieces (5 bits); we subtracted 1 during encoding to allow 32 pieces using 5 bits, here we need to add 1
    int num_pieces = reader.read(5) + 1;

    // 3. Read Each Piece (10 bits: 6 sq + 1 col + 3 type)
    for (int i = 0; i < num_pieces; ++i) {
        int square = reader.read(6);
        int color  = reader.read(1);
        int type   = reader.read(3);
        
        board->piecesOnSquares[square] = PC(color, type);
        board->side[color] |= (1ULL << square);
        board->pieceTypes[type - 1] |= (1ULL << square);
    }

    // 4. Global State
    board->sideToMove = reader.read(1);
    uint8_t castling = reader.read(4);
    if (castling & 1) board->castlingRook[0][0] = FileH;
    else board->castlingRook[0][0] = FileNone;
    if (castling & 2) board->castlingRook[0][1] = FileA;
    else board->castlingRook[0][1] = FileNone;
    if (castling & 4) board->castlingRook[1][0] = FileH;
    else board->castlingRook[1][0] = FileNone;
    if (castling & 8) board->castlingRook[1][1] = FileA;
    else board->castlingRook[1][1] = FileNone;
    
    board->enPassant = reader.read(4);

    // 5. Eval (16 bits)
    // We must cast to int16_t to interpret the bits as a signed number
    uint16_t raw_eval = static_cast<uint16_t>(reader.read(16));
    *eval_cp = static_cast<int16_t>(raw_eval); //eval is from white point of view

    // 6. Align reader (discard padding)
    reader.align();

    return true;
}
        
 
// Helper to find all split files matching pattern "lichess_db_eval(_\d+)?\.bin"
std::vector<std::string> get_data_files(const std::string& base_path) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    
    // Check if base file exists
    if (fs::exists(base_path)) files.push_back(base_path);

    // Check for numbered splits: base_1.bin, base_2.bin...
    // We assume the extension is .bin and the prefix is everything before .bin
    std::string path_no_ext = base_path.substr(0, base_path.find_last_of("."));
    
    int index = 1;
    while (true) {
        std::string next_file = path_no_ext + "_" + std::to_string(index) + ".bin";
        if (fs::exists(next_file)) {
            files.push_back(next_file);
            index++;
        } else {
            break; 
        }
    }
    return files;
}

int main() {
    init_magic_bitboards();
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
    NNUEContext ctx;
    init_nnue_context(&ctx);
    
    //const std::string base_data_path = "../lichess_db_eval.bin";
    const std::string test_data_path = "../Downloads/lichess_db_broadcast_2025-12.bin";

    //auto file_list = get_data_files(base_data_path);
    //std::cout << "Found " << file_list.size() << " data files." << std::endl;
        
    if (std::filesystem::exists(test_data_path)) {
        std::cout << "Opening file: " << test_data_path << "..." << std::endl;
        std::ifstream file(test_data_path, std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Could not open file!");

        BitReader reader(file);
        Board board = {};
        int eval_cp = 0;
        int cp_min = 1000;
        int cp_max = -1000;
        
        int count = 0;
        std::vector<std::pair<Board, int>> samples;
        while (read_next_position(reader, &board, &eval_cp)) {
            samples.push_back({board, eval_cp});
            cp_min = std::min(cp_min, eval_cp);
            cp_max = std::max(cp_max, eval_cp);
            count++;
            if (count % 1000000 == 0) std::cout << "Loaded " << count << " positions. cp_min " << cp_min << ", cp_max " << cp_max << "\n" << std::flush;
        }
        std::cout << "Finished loading " << samples.size() << " positions. cp_min " << cp_min << ", cp_max " << cp_max << std::endl;
      
        double error = 0.0;
        count = 0;
        for (auto& [board, eval_cp] : samples) {
        		MovesContext movesContext;
        		unsigned long long movesFromSquares[64] = {0};
        		generateMoves(&board, &movesContext, getAttackedSquares(&board, &movesContext), movesFromSquares);
            if (!board.isMate && !board.isStaleMate && !board.isCheck) {
              double res = board.sideToMove == ColorWhite ? evaluate_nnue(&board, 0, &ctx) : -evaluate_nnue(&board, 0, &ctx);
              double err = eval_cp * 0.01 - res;
              err *= err;
              error += err;
              count++;
            }
        }
        double mse = (count > 0) ? (error / count) : 0.0;
        std::cout << "    MSE over " << count << " positions: " << mse << " pawns"<< std::endl;
    } else {
        std::cerr << "Warning: Test file " << test_data_path << " not found. Skipping validation." << std::endl;
    }
    free_nnue_context(&ctx);
    cleanup_nnue();
    cleanup_magic_bitboards();
    return 0;
}