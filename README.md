# libchess

This is a C/C++ chess library with python bindings (libffi). One may prefer python ctypes module instead of libffi. This library is built on top of the one that wrote in C# few years ago for MS Windows chess game analyzer available at https://chessgame-analyzer.creatica.org. Additionally, this library uses magic bitboards instead of a linear approach for move generation of ray pieces, supports Syzygy tables for end games, uses Uthash for caching data, has CNN and transformer libtorch-based chess AI model(s), which can be trained and tested on PGN files as well as a very basic UCI AI-based chess engine with a MCTS algorithm to test the model in UCI-capable chess GUI. 

To build with cmake to produce Makefile. To run eval.py, you may need a chess engine such as stockfish. There are other python scripts such as genEndGames.py that will produce a PGN file that could be used for training an AI model. 

```
cmake .
make
gcc -E libchess.h > libchess.ph
(install libffi)
python3 tasks.py
export LD_LIBRARY_PATH=.
python3 eval.py
```
To compile without cmake using clang (works on MacOS):

For chess library:
```
cc -Wno-strncat-size -O3 -Xclang -fopenmp -Wl,-dylib,-lsqlite3,-lomp,-rpath,/opt/anaconda3/lib -I /opt/anaconda3/include -L/opt/anaconda3/lib -o libchess.so bitscanner.c board.c engine.c fen.c game.c game_omp.c move.c piece.c square.c tag.c zobrist-hash.c sqlite.c my_md5.c magic_bitboards.c boards_legal_moves8.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp nnue/nnue/nnue_misc.cpp
```

Please notice dependencies such as OMP. It's mainly used in game_omp.c, which meant to preprocess PGN chess data for AI model training and inference. If you don't plan to use AI, then OMP is not needed, just drop the game_omp.c file from the line and from CMakeLists.txt.

Do not forget to run init_magic_bitboards() at start and cleanup_magic_bitboards() at the end in your programs for ray piece move generation. Otherwise, segfault is guaranteed. 

For chess AI model training:
```
c++ -O3 -I <path_to_libtorch>/libtorch/include -I <path_to_libtorch>/libtorch/include/torch/csrc/api/include -L <path_to_libtorch>/libtorch/lib -L <path_to_libchess> -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,<path_to_libtorch>/libtorch/lib,-rpath,<path_to_libchess> -o chess_cnn train_chess_cnn.cpp chess_cnn.cpp
```
Please notice dependencies, i.e. libtorch. You would need to provide a path to PGN files for training and PGN files for testing in train_chess_cnn.cpp. You may try simplified model in chess_cnn6.cpp and train_chess_cnn6.cpp or heavier ones such as chess_trans.cpp and train_chess_trans.cpp or train_chess_enc_dec.cpp. You may need to select which boards_legal_movesX.c file to use for which model when making libchess.so. 

game.c file is far from being perfect. It has mostly functions to work with PGN files, SQLite3 databases (sqlite.c), generate end games, etc. Possibly, it will be removed from the library and used for various tools in a future.  

For UCI chess engine that uses chess AI model with pre-trained weights and MCTS when close to endgame or Syzygy tables for moves when the number of pieces is equal or less than 5 (tables of 1 GB in size):
```
c++ -Wno-deprecated-declarations -Wno-deprecated -O3 -I <path_to_libtorch>/libtorch/include -I <path_to_libtorch>/libtorch/include/torch/csrc/api/include -I <path_to_libchess> -L <path_to_libtorch>/libtorch/lib -L <path_to_libchess> -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,<path_to_libtorch>/libtorch/lib,-rpath,<path_to_libchess> chess_cnn_mcts.cpp uci.cpp chess_cnn.cpp tbcore.c tbprobe.c -o chess_engine
```
You would need to download Syzygy tables and update TB_MAX_PIECES in uci.cpp before compiling chess_engine.

To comply with licensing of Fathom Syzygy Table Bases:

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
