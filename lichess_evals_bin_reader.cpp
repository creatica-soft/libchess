// c++ -std=c++20 -O3 lichess_evals_bin_reader.cpp -o lichess_evals_bin_reader
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <iomanip>
//1R4k1/3q1pp1/6n1/b2p2Pp/2pP2b1/p1P5/P1BQrPPB/5NK1 b - - 0 1
// --- Definitions to match the binary format ---
enum PieceType { NO_PIECE=0, PAWN=1, KNIGHT=2, BISHOP=3, ROOK=4, QUEEN=5, KING=6 };
enum Color { WHITE=0, BLACK=1 };

// Helper to map pieces to integer values for the array
// Mapping: White (1-6), Black (9-14) [Type | (Color << 3)]
int get_piece_value(int type, int color) {
    return type | (color << 3); 
}

char * get_castling(int castling_rights, char * castling) {
  int count = 0;
  if (castling_rights & 1) castling[count++] = 'K';
  if (castling_rights & 2) castling[count++] = 'Q';
  if (castling_rights & 4) castling[count++] = 'k';
  if (castling_rights & 8) castling[count++] = 'q';
  if (count) castling[count] = 0;
  return castling;
}

// Struct to hold the full position info
struct CompressedPosition {
    int piecesOnSquares[64];
    int side_to_move;    // 0=White, 1=Black
    int castling_rights; // Bitmask
    int ep_file;         // 0-7, 8=None
    int eval_cp;         // Centipawns
};

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

bool read_next_position(BitReader& reader, CompressedPosition& pos, int count) {
    if (reader.eof()) return false;

    // 1. Clear the board
    std::fill(std::begin(pos.piecesOnSquares), std::end(pos.piecesOnSquares), 0);

    // 2. Read Number of Pieces (5 bits), we subtractedd 1 during encoding to allow 32 pieces using 5 bits, here we need to add 1
    int num_pieces = reader.read(5) + 1;

    // 3. Read Each Piece (10 bits: 6 sq + 1 col + 3 type)
    for (int i = 0; i < num_pieces; ++i) {
        int square = reader.read(6);
        int color  = reader.read(1);
        int type   = reader.read(3);
        
        if (square < 64) {
          if (type == NO_PIECE) {
            std::cerr << "get_piece_value() error: type is NO_PIECE. num_pieces " << num_pieces << " square " << square << " color " << color << " pos count " << count + 1 << ". Exiting...\n" << "\n";
            exit(-1);
          };  
          pos.piecesOnSquares[square] = type | (color << 3); //get_piece_value(type, color);
        } else { //this is impossible for 6 bits
          std::cerr << "read_next_position() error: square >= 64\n";
          exit(-1);
        }
    }
    // 4. Global State
    pos.side_to_move    = reader.read(1);
    pos.castling_rights = reader.read(4);
    pos.ep_file         = reader.read(4);

    // 5. Eval (16 bits)
    // We must cast to int16_t to interpret the bits as a signed number
    uint16_t raw_eval = static_cast<uint16_t>(reader.read(16));
    pos.eval_cp = static_cast<int16_t>(raw_eval);

    // 6. Align reader (discard padding)
    reader.align();

    return true;
}

// --- Demo Main ---
void print_board(const int pieces[64]) {
    const char* piece_chars = " PNBRQK  pnbrqk";
    std::cout << "  +-----------------+\n";
    for (int r = 7; r >= 0; --r) {
        std::cout << (r + 1) << " | ";
        for (int f = 0; f < 8; ++f) {
            int sq = r * 8 + f;
            int p = pieces[sq];
            // Decode back to index for char lookup
            // White: 1-6 -> 1-6. Black: 9-14 -> 9-14.
            // Adjusting index to match string:
            // White (1-6), Black (8-13 in string index terms? No, let's map manually)
            char c = '.';
            if (p > 0) {
                int type = p & 7;
                int color = (p >> 3) & 1;
                c = piece_chars[type + (color * 8)]; // Logic: type(1..6) + color*8
            }
            std::cout << c << " ";
        }
        std::cout << "|\n";
    }
    std::cout << "  +-----------------+\n";
    std::cout << "    a b c d e f g h\n";
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " binary_db.bin" << std::endl;
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "Cannot open file: " << argv[1] << std::endl;
        return 1;
    }

    BitReader reader(in);
    CompressedPosition pos;
    
    int count = 0;
    while (read_next_position(reader, pos, count)) {
        // Print first 10 positions to verify
        if (count > 2602011) {
            std::cout << "Position " << count + 1 << ":\n";
            std::cout << "Eval: " << pos.eval_cp << " cp\n";
            std::cout << "Side: " << (pos.side_to_move == 0 ? "White" : "Black") << "\n";
            char castling[5] = "-";
            std::cout << "Castling: " << get_castling(pos.castling_rights, castling) << "\n";
            std::cout << "EP File: " << pos.ep_file << "\n";
            print_board(pos.piecesOnSquares);
            std::cout << "-----------------------\n";
        }
        count++;
    }

    std::cout << "Total positions read: " << count << std::endl;
    return 0;
}