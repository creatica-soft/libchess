//Instead of a flat list, we build a temporary mini-tree in RAM. This allows us to propagate the scores back up (Minimax/Negamax) easily after the GPU batch is done.
struct MiniNode {
    // Tree topology
    std::vector<MiniNode> children;
    
    // For leaves (Eval)
    int tensor_index = -1;       // Index in the flat batch_tensors list
    double static_score = 0.0;   // Score if terminal (Mate/Draw) or Repetition
    bool is_terminal = false;
    
    // For branches (Move info to pass back to root)
    int src, dst, promo;         // The move that created this node
    unsigned long long hash;     // Hash of position at this node
};


//This replaces your loops. It uses depth to switch between "Full Moves" and "Captures Only".
void gather_recursive(Board* board, MiniNode& node, std::vector<torch::Tensor>& batch_tensors, 
                      std::unordered_set<unsigned long long>& pos_history, int depth_left, bool only_captures) {
    // 1. BASE CASE: Stop if depth reached (Evaluated by NN)
    if (depth_left <= 0) {
        batch_tensors.push_back(get_cnn_input(board));
        node.tensor_index = batch_tensors.size() - 1;
        return;
    }

    // 2. GENERATE MOVES
    MovesContext mctx;
    // CRITICAL: Generate moves (Full or Captures Only + checks based on flag)
    if (only_captures) {
        generateCaptures(board, &mctx, getAttackedSquares(board, &mctx)); //should include en passant
    } else {
        generateMoves(board, &mctx, getAttackedSquares(board, &mctx));
    }

    // Handle Mate/Stalemate/No-Moves logic
    if (board->moves == 0) {
        node.is_terminal = true;
        if (only_captures) {
            // If we are in Quiescence search and have no captures, 
            // we typically "stand pat" (evaluate current pos).
            batch_tensors.push_back(get_cnn_input(board));
            node.tensor_index = batch_tensors.size() - 1;
            node.is_terminal = false; 
        } else {
            // Real terminal state (Mate or Stalemate)
            if (board->isMate) node.static_score = -1.0; // We (side to move) lost
            else node.static_score = 0.0; // Draw
        }
        return;
    }

    // 3. RECURSIVE STEP
    int moves_processed = 0;
    
    int side = PC(board->fen->sideToMove, PieceTypeAny);
    unsigned long long any = board->occupations[side];
    while (any) {
        int src = lsBit(any);
        unsigned long long moves = board->movesFromSquares[src];
        if (only_captures) {
             unsigned long long opp = board->occupations[side ^ 1];
             moves &= opp; 
             // Note: This misses En Passant, check your engine's specific capture mask logic
        }
        while (moves) {
            int dst = lsBit(moves);
            int startP = PieceTypeNone, endP = PieceTypeNone;
            if (promoMove(board, src, dst)) { startP = Knight; endP = Queen; }

            for (int pt = startP; pt <= endP; pt++) {
                Board* tmp = cloneBoard(board);
                Move m;
                ff_move(tmp, &m, src, dst, pt);
                updateHash(tmp, &m);

                bool is_rep = (pos_history.count(tmp->zh->hash) > 0);
                
                // Add child node
                node.children.emplace_back();
                MiniNode& child = node.children.back();
                child.src = src; child.dst = dst; child.promo = pt;
                child.hash = tmp->zh->hash;

                if (is_rep) {
                    child.is_terminal = true;
                    child.static_score = 0.0; // Draw
                } else {
                    // Update History for recursion
                    // Copying set is expensive! 
                    // Optimization: Insert, Recurse, Erase (Backtracking)
                    pos_history.insert(tmp->zh->hash);
                    
                    // DECIDE NEXT DEPTH STRATEGY
                    // Example: Depth 0 (Root) -> Full
                    //          Depth 1 (Resp) -> Full
                    //          Depth 2+       -> Captures Only
                    bool next_only_captures = (depth_left - 1 <= 1) ? true : false;
                    
                    // Or strict N-ply:
                    // bool next_only_captures = only_captures; 

                    gather_recursive(tmp, child, batch_tensors, pos_history, depth_left - 1, next_only_captures);
                    
                    pos_history.erase(tmp->zh->hash); // Backtrack
                }
                
                freeBoard(tmp);
                moves_processed++;
            }
            moves &= moves - 1;
        }
        any &= any - 1;
    }
    
    // Corner case: If 'only_captures' yielded 0 moves, we must evaluate the static position
    // (The "Stand Pat" rule in Quiescence Search)
    if (only_captures && moves_processed == 0) {
        batch_tensors.push_back(get_cnn_input(board));
        node.tensor_index = batch_tensors.size() - 1;
        node.is_terminal = false;
        node.children.clear(); // Ensure it acts as leaf
    }
}

//This function walks the tree after the GPU has filled cnn_results. It propagates values up.
//Negamax Principle: My value = max( -child_value ).
double resolve_minimax(const MiniNode& node, const std::vector<float>& cnn_results) {
    // 1. LEAF NODE: Return evaluation
    if (node.children.empty()) {
        if (node.is_terminal) return node.static_score;
        
        // Get NN score (Perspective of Side-To-Move at this leaf)
        double score = static_cast<double>(cnn_results[node.tensor_index]);
        return score;
    }

    // 2. INTERNAL NODE: Negamax recursion
    double best_score = -99999.0; // -Infinity
    
    for (const auto& child : node.children) {
        // Recurse: Get score relative to child's side-to-move
        double child_val = resolve_minimax(child, cnn_results);
        
        // Flip perspective: Child's advantage is My disadvantage
        double my_val = -child_val;
        
        if (my_val > best_score) {
            best_score = my_val;
        }
    }
    
    return best_score;
}

//Now we just set the depth. A depth of 3 with the capture logic above usually yields 2k-4k tensors.
void compute_move_evals(Board* chess_board, SimpleCNN& model,
                        std::unordered_set<unsigned long long>& pos_history, 
                        std::vector<std::tuple<double, int, int, unsigned long long>>& move_evals, 
                        double prob_mass) {
    // 1. Setup Root Node
    MiniNode root;
    std::vector<torch::Tensor> batch_tensors;
    batch_tensors.reserve(5000); // Reserve more for 3-ply

    // 2. Gather Phase (The Simulation)
    // Depth 3: Root(Full) -> Opp(Full) -> Me(Captures) -> Eval Opp
    // You can tune '3' to '4' if M1 is still hungry.
    gather_recursive(chess_board, root, batch_tensors, pos_history, 3, false);
       
    // 3. Inference (The GPU Crunch)
    std::vector<float> cnn_results;    
    if (!batch_tensors.empty()) {
        // Optimization: Process in chunks if batch > 5000 to avoid VRAM spike
        auto cpu_batch = torch::stack(batch_tensors);
        auto batch_input = cpu_batch.to(device);                
        {
            torch::NoGradGuard no_grad; 
            auto output = model->forward(batch_input);
            auto output_cpu = output.cpu();            
            float* data_ptr = output_cpu.data_ptr<float>();
            cnn_results.assign(data_ptr, data_ptr + batch_tensors.size());
        }
    }

    // 4. Resolve & Map to Move List
    // The root's children are the legal moves we want to populate in move_evals
    for (const auto& child : root.children) {
        // We evaluate the child branch
        double val = resolve_minimax(child, cnn_results);
        
        // Negamax: The value returned is from Child's perspective (Opponent).
        // We want Root's perspective.
        // resolve_minimax returns "Score for SideToMove at Child".
        // Child SideToMove = Opponent.
        // So we want -val.
        double final_score = -val;

        move_evals.push_back({
            final_score, 
            (child.src << 9) | (child.dst << 3) | child.promo, 
            static_cast<int>(final_score * 100), 
            child.hash
        });
    }

    // 5. Sort & Prune
    std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    int effective_branching = get_prob(move_evals, prob_mass);
    move_evals.resize(effective_branching);
}





#include <future>
#include <vector>
#include <queue>

// A request object representing one worker's batch
struct InferenceRequest {
    std::vector<torch::Tensor> tensors;
    std::promise<std::vector<float>> promise; // To return results
};
//This class manages the GPU. It allows workers to "submit and wait".
class PredictionServer {
public:
    PredictionServer(SimpleCNN& model, torch::Device device) 
        : model_(model), device_(device), running_(true) {
        // Start the dedicated GPU thread
        server_thread_ = std::thread(&PredictionServer::server_loop, this);
    }

    ~PredictionServer() {
        running_ = false;
        cv_.notify_all();
        if (server_thread_.joinable()) server_thread_.join();
    }

    // Called by Worker Threads
    std::vector<float> predict(std::vector<torch::Tensor>& tensors) {
        if (tensors.empty()) return {};

        std::promise<std::vector<float>> prom;
        auto fut = prom.get_future();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            requests_.push({std::move(tensors), std::move(prom)});
        }
        cv_.notify_one(); // Wake up the server

        // Block until the server processes our batch
        return fut.get();
    }

private:
    void server_loop() {
        // Clone model for this thread (MPS requirement)
        SimpleCNN local_model = clone_model(model_, device_);
        std::vector<InferenceRequest> batch_buffer;

        while (running_) {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            // Wait for requests or timeout (to process whatever we have)
            // waiting 1-2ms helps aggregate more requests from other threads
            cv_.wait_for(lock, std::chrono::milliseconds(1), [&]{ 
                return !requests_.empty() || !running_; 
            });

            if (!running_ && requests_.empty()) break;

            // Drain the ENTIRE queue into our buffer
            while (!requests_.empty()) {
                batch_buffer.push_back(std::move(requests_.front()));
                requests_.pop();
            }
            lock.unlock();

            if (batch_buffer.empty()) continue;

            // --- 1. MERGE ---
            std::vector<torch::Tensor> global_batch;
            // Pre-calculate total size to avoid reallocations
            size_t total_size = 0;
            for (const auto& req : batch_buffer) total_size += req.tensors.size();
            global_batch.reserve(total_size);

            for (const auto& req : batch_buffer) {
                global_batch.insert(global_batch.end(), req.tensors.begin(), req.tensors.end());
            }

            // --- 2. INFERENCE (One Huge Batch) ---
            std::vector<float> global_results;
            if (!global_batch.empty()) {
                 auto input = torch::stack(global_batch).to(device_);
                 torch::NoGradGuard no_grad;
                 auto output = local_model->forward(input);
                 
                 // MPS Optimization: Copy to CPU once
                 auto output_cpu = output.cpu();
                 float* ptr = output_cpu.data_ptr<float>();
                 global_results.assign(ptr, ptr + total_size);
            }

            // --- 3. DISTRIBUTE RESULTS ---
            size_t offset = 0;
            for (auto& req : batch_buffer) {
                size_t count = req.tensors.size();
                std::vector<float> worker_results(
                    global_results.begin() + offset, 
                    global_results.begin() + offset + count
                );
                req.promise.set_value(std::move(worker_results));
                offset += count;
            }

            batch_buffer.clear();
        }
    }

    SimpleCNN& model_;
    torch::Device device_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    
    std::queue<InferenceRequest> requests_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
};
// Add a global or engine-level pointer
// PredictionServer* g_prediction_server = nullptr;


struct MiniNode {
    std::vector<MiniNode> children;
    
    // Graph/Batch links
    int tensor_index = -1;       // Index in global batch
    int turn;                    // WHITE or BLACK (The side to move at this node)
    
    // Evaluation
    double static_score = 0.0;   // Score if terminal (Mate/Draw)
    bool is_terminal = false;
    
    // Move from parent to here
    int src, dst, promo;         
    unsigned long long hash;     
};

double resolve_negamax(const MiniNode& node, const std::vector<float>& cnn_results) {
    if (node.children.empty()) {
        if (node.is_terminal) return node.static_score;
        float raw_val = cnn_results[node.tensor_index]; 
        return static_cast<double>(raw_val);
    }
    double best_score = -99999.0;    
    for (const auto& child : node.children) {
        double child_val = resolve_negamax(child, cnn_results);
        double my_val = -child_val;        
        if (my_val > best_score) best_score = my_val;
    }
    return best_score;
}

void gather_recursive(Board * chess_board, Fen * board_fen, ZobristHash * board_hash, MiniNode& node, std::vector<torch::Tensor>& batch_tensors, std::unordered_set<unsigned long long>& pos_history, int depth, int side_to_move) {
    node.turn = side_to_move; // Record who moves next (and who evaluates)

    // --- DECISION LOGIC ---
    // 1. Hard Stop: If we hit max depth, we must evaluate.
    // 2. Q-Search Trigger: If depth >= 2, we switch to captures only.
    bool only_captures = (depth >= 2); 
    bool hard_stop = (depth >= 5); // Prevent explosion

    if (hard_stop) {
        batch_tensors.push_back(get_cnn_input(board));
        node.tensor_index = batch_tensors.size() - 1;
        return;
    }

    // --- MOVE GENERATION ---
    MovesContext mctx;
    generateMoves(chess_board, board_fen, &mctx, getAttackedSquares(chess_board, board_fen, &mctx));

    // --- TERMINAL CHECKS ---
    // If no moves available (Mate/Stalemate OR Quiet position in Q-Search)
    if (mctx.move_cnt == 0) {
        if (only_captures) {
            // Stand Pat: In Q-Search, running out of captures is not "Game Over", 
            // it just means the position is quiet. Evaluate it.
            batch_tensors.push_back(get_cnn_input(chess_board));
            node.tensor_index = batch_tensors.size() - 1;
        } else {
            // Real Terminal (Mate or Stalemate)
            node.is_terminal = true;
            if (chess_board->isMate) node.static_score = -1.0; // Current side lost
            else node.static_score = 0.0; // Draw
        }
        return;
    }

    // --- RECURSION ---
    int moves_processed = 0;
    
    int side = PC(board_fen->sideToMove, PieceTypeAny);
    unsigned long long any = chess_board->occupations[side];
    while (any) {
        int src = lsBit(any);
        unsigned long long moves = chess_board->movesFromSquares[src];
        if (only_captures) {
             unsigned long long opp = chess_board->occupations[side ^ 1];
             moves &= opp; 
             // Note: This misses En Passant, check your engine's specific capture mask logic
        }

        // FILTER: If only_captures, mask 'moves' with opponent pieces
        if (only_captures) {
          unsigned long long opp = chess_board->occupations[side ^ 1];
          moves &= opp; 
      		//plus en passant pawn moves
    			if (PC_TYPE(chess_board->piecesOnSquares[src]) == Pawn) {
    			 	const signed char pawnShifts[3][3] = { { 8, 7, 9 }, { -8, -9, -7 } }; // { { N, NW, NE}, {S, SW, SE} }
          	const int pawnRanks[3][3] = { { Rank2, Rank5, Rank6 }, { Rank7, Rank4, Rank3 } };	
  					const int srcFile = SQ_FILE(src);
        		const int srcRank = SQ_RANK(src);
      			unsigned long long d = 0;
      			int shift;
      			if (srcFile > FileA) {
      				shift = src + pawnShifts[board_fen->sideToMove][1]; //NW (white) or SW (black)
              unsigned long long bit_src = SQ_BIT(shift);
      				if ((board_fen->enPassant == (srcFile - 1)) && (srcRank == pawnRanks[board_fen->sideToMove][1])) { //rank5 or rank4
      					//make sure there is no discover check from a queen or a rook
      					chess_board->piecesOnSquares[src] = PieceNameNone; //temporarily remove moving pawn
      					chess_board->piecesOnSquares[src - 1] = PieceNameNone; //temporarily remove en passant pawn
      					if (enPassantLegalBit(chess_board, board_fen)) d |= bit_src;
      					chess_board->piecesOnSquares[src] = PC(board_fen->sideToMove, Pawn); //restore moving pawn
      					chess_board->piecesOnSquares[src - 1] = PC(OPP_COLOR(board_fen->sideToMove), Pawn); //restore en passant pawn
      				}
      			}
      			if (srcFile < FileH) {
      				shift = src + pawnShifts[board_fen->sideToMove][2]; //NE (white) or SE (black)
              unsigned long long bit_src = SQ_BIT(shift);
      				if ((board_fen->enPassant == (srcFile + 1)) && (srcRank == pawnRanks[board_fen->sideToMove][1])) {
      					//make sure there is no discover check from a queen or a rook
      					chess_board->piecesOnSquares[src] = PieceNameNone; //temporarily remove moving pawn
      					chess_board->piecesOnSquares[src + 1] = PieceNameNone; //temporarily remove en passant pawn
      					if (enPassantLegalBit(chess_board, board_fen)) d |= bit_src;
      					chess_board->piecesOnSquares[src] = PC(board_fen->sideToMove, Pawn); //restore moving pawn
      					chess_board->piecesOnSquares[src + 1] = PC(OPP_COLOR(board_fen->sideToMove), Pawn); //restore en passant pawn
      				}
      			}
            moves |= d;
          }
        }
        while (moves) {
            int dst = lsBit(moves);
            int startP = PieceTypeNone, endP = PieceTypeNone;
            if (promoMove(chess_board, board_fen, src, dst)) { startP = Knight; endP = Queen; }

            for (int pt = startP; pt <= endP; pt++) {
                Board tmp_board = *chess_board;
                Fen tmp_fen = *board_fen;
                ZobristHash tmp_hash = *board_hash;
                Move move;
                ff_move(&move, &tmp_board, &tmp_fen, src, dst, promo);
                updateHash(&move, &tmp_board, &tmp_fen);
                bool is_rep = (pos_history.count(tmp_hash.hash) > 0);                
                node.children.emplace_back();
                MiniNode& child = node.children.back();
                child.src = src; child.dst = dst; child.promo = promo;
                child.hash = tmp_hash.hash;
                if (is_rep) {
                    child.is_terminal = true;
                    child.static_score = 0.0; 
                    child.turn = side_to_move ^ 1; // It would have been opponent's turn
                } else {
                    pos_history.insert(tmp_hash.hash);
                    // Recurse: Depth + 1, Flip Side
                    gather_recursive(&tmp_board, &tmp_fen, &tmp_hash, child, batch_tensors, pos_history, depth + 1, side_to_move ^ 1);
                    pos_history.erase(tmp_hash.hash);
                }
                moves_processed++;
            }
        }
    }

    // STAND PAT (Again): 
    // If we were in Q-Search (only_captures) and generated moves, 
    // we technically should ALSO allow "Standing Pat" (not capturing).
    // In Minimax Q-Search, this is handled by taking max(static_eval, best_capture).
    // For simplicity with NN batching, we usually just force the tree to expand.
    // If you want true Stand Pat, you would add a "null child" that evaluates CURRENT board.
    // But for 2-3 ply batches, strictly following captures is usually fine.
}

void compute_move_evals(Board* chess_board, Fen * board_fen, ZobristHash * board_hash,
                        PredictionServer* server, // Pass the server instead
                        std::unordered_set<unsigned long long>& pos_history, 
                        std::vector<std::tuple<double, int, int, unsigned long long>>& move_evals, 
                        double prob_mass) {
    MiniNode root;
    int root_side = PC(board_fen->sideToMove, PieceTypeAny);
    
    // 1. CPU HEAVY WORK: Generate thousands of tensors
    // This runs in parallel across your 7 worker threads
    std::vector<torch::Tensor> batch_tensors;
    gather_recursive(chess_board, board_fen, board_hash, root, batch_tensors, pos_history, 0, root_side);
    
    // 2. SERVER REQUEST: Submit and Wait
    // The server will combine this with requests from other threads
    std::vector<float> cnn_results = server->predict(batch_tensors);

    // 3. RESOLVE (CPU)
    for (const auto& child : root.children) {
        double val = resolve_negamax(child, cnn_results);
        move_evals.push_back({ -val, (child.src << 9) | (child.dst << 3) | child.promo, static_cast<int>(-val * 100), child.hash});
    }
    std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    int effective_branching = get_prob(move_evals, prob_mass);
    move_evals.resize(effective_branching);
}