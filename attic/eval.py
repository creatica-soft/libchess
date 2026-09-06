import sys
from chess import ffi, lib

if __name__ == "__main__":
  engineName = ffi.new('char[]', b'/Users/ap/libchess/stockfish-macos-m1-apple-silicon')
  movetime = 5000 # in ms
  depth = 20 # in plies
  hashSize = 16 # in MB
  threadNumber = 1
  syzygyPath = b''
  multiPV = 2
  chessEngine = lib.initChessEngine(engineName, movetime, depth, hashSize, threadNumber, syzygyPath, multiPV)
  if chessEngine == ffi.NULL:
    print('initChessEngine() returned NULL')
    ffi.release(engineName)
    quit()
  chessEngine.position = b''
  chessEngine.moves = b'e2e4'
  eval1 = ffi.new('struct Evaluation *')
  eval2 = ffi.new('struct Evaluation *')
  eval1.maxPlies = 16
  eval2.maxPlies = 16
  lib.isReady(chessEngine)
  lib.newGame(chessEngine)
  lib.position(chessEngine)
  lib.go(chessEngine, [eval1, eval2])
  print(f' best move {ffi.string(eval1.bestmove).decode()}, NAG {eval1.nag}', file=sys.stderr)
  print(f' variation 1: {ffi.string(eval1.pv).decode()}', file=sys.stderr)
  print(f' variation 2: {ffi.string(eval2.pv).decode()}', file=sys.stderr)
  lib.quit(chessEngine)
  ffi.release(eval1)
  ffi.release(eval2)
  lib.releaseChessEngine(chessEngine)
  ffi.release(engineName)
