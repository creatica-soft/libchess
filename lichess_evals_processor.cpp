//c++ -std=c++20 -O3 -flto lichess_evals_processor.cpp -o lichess_evals_processor
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include "json.hpp"

//lichess_db_eval.jsonl format
//{"fen":"7r/1p3k2/p1bPR3/5p2/2B2P1p/8/PP4P1/3K4 b - -","evals":[{"pvs":[{"cp":48,"line":"f7g7 e6e2 h8d8 e2d2 b7b5 c4b3 a6a5 a2a3 g7f6 b3a2"}],"knodes":644403,"depth":55},{"pvs":[{"cp":69,"line":"f7g7 e6e2 h8d8 e2d2 b7b5 c4b3 g7f6 d1e1 a6a5 a2a3"},{"cp":163,"line":"h8d8 d1e1 a6a5 a2a3 c6d7 e6e7 f7f6 e1f2 b7b5 c4b3"},{"cp":229,"line":"h8a8 d1e1 a6a5 e6h6 f7g7 h6h4 a8d8 c4d3 c6g2 d3f5"},{"cp":231,"line":"h8f8 d1e1 b7b5 c4b3 a6a5 e6h6 f7g7 h6h4 f8e8 e1f2"},{"cp":237,"line":"h8b8 d1e1 a6a5 e6h6 f7g7 h6h4 b8d8 c4d3 c6g2 d3f5"}],"knodes":4189972,"depth":46}]}
//or with mate
//{"fen":"6k1/4Rppp/8/8/8/8/5PPP/6K1 w - -","evals":[{"pvs":[{"mate":1,"line":"e7e8"}],"knodes":154,"depth":99},{"pvs":[{"mate":1,"line":"e7e8"},{"mate":23,"line":"g1f1 g7g6 e7a7 g8g7 f1e2 g7f6 e2e3 f6e6 e3e4 f7f5"},{"mate":23,"line":"e7a7 g7g6 g1f1 g8g7 f1e2 g7f6 e2e3 f6e6 e3e4 h7h5"},{"mate":24,"line":"e7b7 g7g5 g1f1 g8g7 f1e2 g7f6 e2e3 f6e6 b7b6 e6f5"},{"mate":24,"line":"e7c7 g7g6 g1f1 g8g7 f1e2 g7f6 e2e3 f6e6 e3e4 f7f5"}],"knodes":1710146,"depth":70},{"pvs":[{"mate":1,"line":"e7e8"},{"cp":8308,"line":"g1f1 g7g6 f1e2 g8f8 e7a7 f8g7 e2f3 g7f6 f3e3 f6e6"},{"cp":8308,"line":"e7e1 g7g6 g1f1 g8g7 e1a1 g7f6 a1a5 f6e6 f1e2 e6d6"},{"cp":8308,"line":"e7b7 g7g6 g1f1 g8g7 f1e2 g7f6 e2e3 f6e6 e3e4 e6f6"},{"cp":8308,"line":"e7d7 g7g6 d7a7 g8g7 g1f1 g7f6 f1e2 f6e6 a7a5 e6f6"},{"cp":8308,"line":"e7a7 g7g6 g1f1 g8g7 a7a6 g7h6 f1e2 h6g5 e2f3 g5f5"},{"cp":8308,"line":"e7e5 g7g6 g1f1 g8g7 f1e2 g7f6 e5a5 f6e6 e2e3 e6f6"},{"cp":8307,"line":"e7c7 g8f8 g1f1 f8e8 f1e2 e8f8 e2f3 f8e8 f3f4 e8f8"},{"cp":8306,"line":"e7e4 h7h6 g1f1 g8h7 f1e2 h7g6 e4a4 g6h5 a4a5 h5g4"},{"cp":741,"line":"f2f3 g7g5 g1f2 g8g7 e7a7 g7f6 f2e3 f6e6 a7a6 e6e7"}],"knodes":1110315,"depth":29}]}

//efficient binary FEN format
//number of pieces - 5 bits
//piece square - 6 bits
//piece color - 1 bit
//piece type - 3 bits
// 10 bits per piece at square x number of pieces + 24 bits for
//side to move - 1 bit
//castling - 4 bits
//en passant - 4 bits
//halfmoon clock - probably not needed
//move number - not needed
//eval in cp - 16 bits
//0-padding to byte boundary

//repeat for next position

class my_sax : public nlohmann::json_sax<nlohmann::json> {
private:
    std::ofstream& out;
    int object_level = 0;
    std::string fen;
    int max_depth;
    struct PV {
        int cp;
        std::string line;
    };
    struct Eval {
        std::vector<PV> pvs;
        std::int64_t knodes;
        int depth;
    };
    Eval best_eval;
    bool in_evals = false;
    Eval current_eval;
    bool in_pvs = false;
    PV current_pv;
    std::string current_key;
    std::string pv_key;

public:
    my_sax(std::ofstream& o) : out(o) {}

    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t val) override {
        if (in_pvs) {
            if (pv_key == "cp") {
                current_pv.cp = static_cast<int>(val);
            }
        } else if (in_evals) {
            if (current_key == "knodes") {
                current_eval.knodes = val;
            } else if (current_key == "depth") {
                current_eval.depth = static_cast<int>(val);
            }
        }
        return true;
    }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool string(string_t& val) override {
        if (in_pvs) {
            if (pv_key == "line") {
                current_pv.line = val;
            }
        } else if (!in_evals) {
            if (current_key == "fen") {
                fen = val;
            }
        }
        return true;
    }
    bool binary(binary_t&) override { return true; }
    bool start_object(std::size_t) override {
        ++object_level;
        if (object_level == 1) {
            // Start of a new position object
            fen = "";
            max_depth = -1;
        } else if (in_evals && !in_pvs) {
            // Start of a new eval object
            current_eval = {{}, 0, 0};
        } else if (in_pvs) {
            // Start of a new pv object
            current_pv = {0, ""};
        }
        return true;
    }
    bool key(string_t& val) override {
        if (in_pvs) {
            pv_key = val;
        } else {
            current_key = val;
        }
        return true;
    }
    bool end_object() override {
        if (in_pvs) {
            // End of pv object
            current_eval.pvs.push_back(current_pv);
        } else if (in_evals && object_level > 1) {
            // End of eval object
            if (current_eval.depth > max_depth) {
                max_depth = current_eval.depth;
                best_eval = current_eval;
            }
        }
        if (object_level == 1) {
            // End of position object, process and output
            if (max_depth == -1) {
                --object_level;
                return true; // No evals, skip
            }
            // Keep only the first PV
            if (!best_eval.pvs.empty()) {
                PV first = best_eval.pvs[0];
                best_eval.pvs = {first};
            }
            // Build the output JSON
            nlohmann::json j;
            j["fen"] = fen;
            //nlohmann::json e = nlohmann::json::object();
            //e["knodes"] = best_eval.knodes;
            //e["depth"] = best_eval.depth;
            //e["pvs"] = nlohmann::json::array();
            if (!best_eval.pvs.empty()) {
                nlohmann::json pv = nlohmann::json::object();
                //pv["cp"] = best_eval.pvs[0].cp;
                j["cp"] = best_eval.pvs[0].cp;
                //pv["line"] = best_eval.pvs[0].line;
                //e["pvs"].push_back(pv);
            }
            //j["evals"] = e;
            out << j.dump() << std::endl;
        }
        --object_level;
        return true;
    }
    bool start_array(std::size_t) override {
        if (current_key == "evals") {
            in_evals = true;
        } else if (current_key == "pvs") {
            in_pvs = true;
            current_eval.pvs.clear();
        }
        return true;
    }
    bool end_array() override {
        if (in_pvs) {
            in_pvs = false;
        } else if (in_evals) {
            in_evals = false;
        }
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::json::exception&) override {
        return false;
    }
};

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.json output.json" << std::endl;
        return 1;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "Cannot open input file: " << argv[1] << std::endl;
        return 1;
    }
    std::ofstream out(argv[2]);
    if (!out) {
        std::cerr << "Cannot open output file: " << argv[2] << std::endl;
        return 1;
    }
    my_sax handler(out);
    bool parse_success = nlohmann::json::sax_parse(in, &handler);
    if (!parse_success) {
        std::cerr << "JSON parse error" << std::endl;
        return 1;
    }
    return 0;
}