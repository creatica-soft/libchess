# libchess

This is a C/C++ chess library with python bindings (libffi). The same (or almost the same) library but written in C# is used in https://chessgame-analyzer.creatica.org. Additionally, it has CNN-based chess AI model, which can be trained and tested on PGN files as well as a simple UCI chess engine to test the model in UCI-capable chess GUI. 

It relies on cmake to produce Makefile. To run eval.py, you may need a chess engine such as stockfish. 

```
cmake .
make
gcc -E libchess.h > libchess.ph
(install libffi)
python3 tasks.py
export LD_LIBRARY_PATH=.
python3 test.py
python3 eval.py
```
To compile without cmake using clang (works on MacOS):

For chess library:
```
cc -Wno-strncat-size -O3 -Xclang -fopenmp -Wl,-dylib,-lsqlite3,-lomp,-rpath,/opt/anaconda3/lib -I /opt/anaconda3/include -L/opt/anaconda3/lib -o libchess.so bitscanner.c board.c engine.c fen.c game.c game_omp.c move.c piece.c square.c tag.c zobrist-hash.c sqlite.c my_md5.c magic_bitboards.c
```
Do not forget to run init_magic_bitboards() and cleanup_magic_bitboards() in your programs for ray piece move generation.

For chess AI model trainig:
```
c++ -O3 -I <path_to_libtorch>/libtorch/include -I <path_to_libtorch>/libtorch/include/torch/csrc/api/include -L <path_to_libtorch>/libtorch/lib -L <path_to_libchess> -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,<path_to_libtorch>/libtorch/lib,-rpath,<path_to_libchess> -o chess_cnn train_chess_cnn.cpp chess_cnn.cpp
```
Please notice dependencies, i.e. libtorch

For UCI chess engine that will use chess AI model with pre-trained weights or Syzygy tables for moves when the number of pieces is equal or less than 5:
```
c++ -Wno-deprecated-declarations -Wno-deprecated -O3 -I <path_to_libtorch>/libtorch/include -I <path_to_libtorch>/libtorch/include/torch/csrc/api/include -I <path_to_libchess> -L <path_to_libtorch>/libtorch/lib -L <path_to_libchess> -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,<path_to_libtorch>/libtorch/lib,-rpath,<path_to_libchess> chess_cnn_mcts.cpp uci.cpp chess_cnn.cpp tbcore.c tbprobe.c magic_bitboards.c -o chess_engine
```

To comply with licensing of Fathom Syzygy Table Bases:

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
