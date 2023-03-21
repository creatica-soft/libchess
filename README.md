# libchess

This is a C chess library with python bindings. The same (or almost the same) library but written in C# is used in https://chessgame-analyzer.creatica.org.

It relies on cmake to compile. To run eval.py, you may need a chess engine such as stockfish. 

```
cmake .
make
gcc -E libchess.h >> libchess.ph
python3 tasks.py
export LD_LIBRARY_PATH=.
python3 test.py
python3 eval.py
```