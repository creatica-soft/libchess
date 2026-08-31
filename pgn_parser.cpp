// c++ -std=c++20 -Wno-writable-strings -O3 -flto -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o pgn_parser pgn_parser.cpp
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_set>
#include "libchess.h"

// --- Data Structures ---
struct MoveData {
    std::string move;      // e.g., "Nf3" (cleaned of ?! etc)
    float eval = 0.0f;     // Stockfish evaluation
    bool has_eval = false; // Is the eval valid?
};

struct PgnGame {
    std::map<std::string, std::string> tags;
    std::vector<MoveData> moves;
};

// --- Parser Class ---
const int mate_score = 20000;
class PgnParser {
public:
    // Reads the next game from the file. Returns false on EOF.
    bool nextGame(std::ifstream& file, PgnGame& game) {
        game.tags.clear();
        game.moves.clear();
        
        std::string line;
        std::string moveSection;
        bool inMoveSection = false;

        // 1. Parse Tags & Accumulate Move Text
        while (std::getline(file, line)) {
            // Trim whitespace (simple check)
            if (line.empty() || line == "\r") {
                if (inMoveSection) break; // End of current game
                continue; 
            }

            if (line[0] == '[') {
                parseTag(line, game.tags);
            } else {
                inMoveSection = true;
                moveSection += line + " ";
            }
        }

        if (moveSection.empty() && game.tags.empty()) return false;

        // 2. Parse the accumulated move text
        if (!moveSection.empty()) {
            parseMoveTokens(moveSection, game);
        }
        
        return true;
    }

private:
    void parseTag(const std::string& line, std::map<std::string, std::string>& tags) {
        std::regex tagRegex(R"(\[\s*(\w+)\s+\"([^\"]*)\"\s*\])");
        std::smatch match;
        if (std::regex_search(line, match, tagRegex)) {
            tags[match[1]] = match[2];
        }
    }

    void parseMoveTokens(const std::string& text, PgnGame& game) {
        // Token Regex: 
        // Group 1: Comments { ... }
        // Group 2: Everything else (Moves, Numbers, Results)
        // [^\s\{\}]+ matches any sequence of chars that isn't whitespace, { or }
        std::regex tokenRegex(R"(\{[\s\S]*?\}|([^\s\{\}]+))");
        
        auto begin = std::sregex_iterator(text.begin(), text.end(), tokenRegex);
        auto end = std::sregex_iterator();

        for (auto i = begin; i != end; ++i) {
            std::string token = i->str();

            if (token[0] == '{') {
                // It's a comment -> Look for eval and assign to LAST move
                if (!game.moves.empty()) {
                    extractEval(token, game.moves.back());
                }
            } 
            else {
                // It's text (Move, Number, or Result)
                if (isResult(token)) break;     // End of game result (1/2-1/2)
                if (isMoveNumber(token)) continue; // e.g., "1." or "1..."
                if (isGlyph(token)) continue; //$18, etc
                
                // It must be a move! Clean it (remove ?!) and store
                std::string clean = cleanMove(token);
                if (!clean.empty()) {
                    MoveData m;
                    m.move = clean;
                    game.moves.push_back(m);
                }
            }
        }
    }

    void extractEval(const std::string& comment, MoveData& lastMove) {
        // Matches [%eval 0.17] or [%eval #3] or [%eval -1.05]
        std::regex evalRegex(R"(\[%eval\s+([#-]?\d+(\.\d+)?)\])");
        std::smatch match;
        if (std::regex_search(comment, match, evalRegex)) {
            std::string valStr = match[1].str();
            
            // Handle Mate scores (e.g., #3)
            if (valStr.find('#') != std::string::npos) {
                // Remove '#'
                valStr.erase(std::remove(valStr.begin(), valStr.end(), '#'), valStr.end());
                int mateVal = std::stoi(valStr);
                // Convert mate to a large finite number (positive or negative)
                lastMove.eval = (mateVal > 0) ? static_cast<float>(mate_score - mateVal) / 100.0f : static_cast<float>(std::abs(mateVal) - mate_score) / 100.0f;
            } else {
                lastMove.eval = std::stof(valStr);
            }
            lastMove.has_eval = true;
        }
    }

    std::string cleanMove(const std::string& raw) {
        std::string clean;
        for (char c : raw) {
            // Keep only valid SAN characters (letters, numbers, +, -, =)
            // Discard !, ?, #
            if (isalnum(c) || c == '-' || c == '=' || c == '+') {
                clean += c;
            }
        }
        return clean;
    }
    
    bool isGlyph(const std::string& s) {
      if (s.front() == '$') return true;
      return false;
    }
 
    bool isMoveNumber(const std::string& s) {
        // Simple check: ends with '.' or contains digit+dot
        if (s.back() == '.') return true; // "1."
        if (s.find(".-") != std::string::npos) return true; // "1.-"
        if (s.find("...") != std::string::npos) return true; // "1..."
        return isdigit(s[0]); // Catches "1." if split weirdly
    }

    bool isResult(const std::string& s) {
        return s == "1-0" || s == "0-1" || s == "1/2-1/2" || s == "*" || s == "½-½" || s == "--+" || s == "+--";
    }
};

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
    uint64_t accumulator; //max 64 bits!
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

int main(int argc, char ** argv) {
    if (argc != 3) {
      std::cerr << "Usage: ./pgn_parser <pgn_file> <bin_file>\n";
      return 1;
    }
    const std::string in = argv[1];
    std::ifstream file(in);
    if (!file.is_open()) {
        std::cerr << "Could not open pgn file " << in << "\n";
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
    std::optional<BitStream> stream;
    stream.emplace(out);
    //BitStream stream(out);

    PgnParser parser;
    PgnGame game;
    int count = 0;
    uint64_t positions = 0;
    uint64_t skipped = 0;
    uint64_t duplicate = 0;
    std::unordered_set<uint64_t> unique_positions(50000000);
    init_magic_bitboards();
    Zobrist z = {};
    zobristHash(z);
    while (parser.nextGame(file, game)) {
      if (game.tags["Variant"] != "Standard") continue;
      //std::cout << "Game " << ++count << ": " << game.tags["White"] << " vs " << game.tags["Black"] << ". Variant " << game.tags["Variant"] << ". Fen " << game.tags["FEN"] << "\n";
      ++count;
      if (count % 10000 == 0) std::cout << "Processed " << count << " games\n" << std::flush;
      //std::cout << "Parsed " << game.moves.size() << " moves.\n";
      Board board = {};
      ZobristHash zh = {};
      if (game.tags["Variant"] == "Chess960") board.isChess960 = true;
      if (game.tags["FEN"].empty())
        fen2board(board, startPos);
      else {
        if (strncmp(game.tags["FEN"].c_str(), startPos, strlen(startPos)) != 0) continue;
        fen2board(board, game.tags["FEN"].c_str());
      }
      getHash(zh, board, z);
      for (const auto& m : game.moves) {
        //std::cout << "  " << m.move << "\n";
        Move move = {};
    		if (san2move(board, m.move.c_str(), move)) {
    		  std::cerr << "san2move() returned non-zero code. Move " << m.move << "; " << game.tags["White"] << " vs " << game.tags["Black"] << ". Variant " << game.tags["Variant"] << ". Fen " << game.tags["FEN"] << "\n";
    		  //cleanup_magic_bitboards();
    		  //exit(1);
    		  skipped++;
    		  break;
    		}
        PieceType pt = ff_move(board, move);
        if (__builtin_popcountll(board.side[0] | board.side[1]) < 6) {
          skipped++;
          break; //use Syzygy tables
        }
        if (m.has_eval) {
          updateHash(zh, board, move, pt, z);
          if (unique_positions.contains(zh.hash)) {
            duplicate++;
          } else {
            unique_positions.insert(zh.hash);
            positions++;
            // [6 bits] Num Pieces
            uint64_t pieces = board.side[ColorWhite] | board.side[ColorBlack];
            stream->write(__builtin_popcountll(pieces) - 1, 5);
           
            // [10 bits per piece]
            while (pieces) {
                int square = __builtin_ctzll(pieces);
                stream->write(square, 6);
                stream->write(PC_COLOR(board.piecesOnSquares[square]), 1);
                stream->write(PC_TYPE(board.piecesOnSquares[square]), 3);
                pieces &= pieces - 1;
            }
           
            // Global State
            stream->write(board.sideToMove, 1);
            const int castlingRights = board.castlingRights;	
            stream->write(castlingRights, 4);
            stream->write(board.enPassant, 4);
           
            // [16 bits] Eval CP
            stream->write(static_cast<uint16_t>(static_cast<int16_t>(m.eval * 100)), 16);
           
            stream->align();
            
            //std::cout << " (Eval: " << m.eval << ")\n";
          } //end of unique
        } //end of if (has eval) 
        else skipped++;
        if (positions % 10000000 == 0) {
          stream->align();
          stream.reset();
          out.close();
          std::cout << "Finished processing " << positions << " positions." << std::endl;
          //return 0;
          part++;
          filename = prefix + "_" + std::to_string(part) + ext; // e.g., output_1.bin
          out.open(filename, std::ios::out | std::ios::binary | std::ios::trunc);
          if (!out) {
              std::cerr << "Cannot open output file: " << filename << std::endl;
              return 1;
          }
          stream.emplace(out);        
        }
      } //end of moves loop
    } //end of games loop
    out.close();
    std::cout << "Finished processing " << positions + skipped + duplicate << " positions. Duplicate " << duplicate << ". No eval info " << skipped << ". Stored " << positions << std::endl;    
    cleanup_magic_bitboards();
    return 0;
}
