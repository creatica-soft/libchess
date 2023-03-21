import sys, time
from chess import ffi, lib

if __name__ == "__main__":
#  fileName = ffi.new('char[]', b'test.pgn')
#  ecoFileName = ffi.new('char[]', b'eco.pgn')
  engineName = ffi.new('char[]', b'stockfish')
  chessEngine = ffi.new('struct Engine *')
  chessEngine.position = b''
  chessEngine.moves = b'e2e4'
  chessEngine.movetime = 5000
  chessEngine.depth = 20
  chessEngine.nodes = 0
  chessEngine.searchmoves = ffi.NULL
  chessEngine.infinite = False
  chessEngine.ponder = False
  chessEngine.mate = 0
  chessEngine.movestogo = 0
  chessEngine.wtime = 0
  chessEngine.btime = 0
  chessEngine.winc = 0
  chessEngine.binc = 0
  res = lib.engine(engineName)
  res = lib.getOptions(chessEngine)
  MultiPV = ffi.new('char[]', b'MultiPV')
  Spin = ffi.cast('enum OptionType', lib.Spin)
  multiPV = lib.nametoindex(chessEngine, MultiPV, lib.Spin)
  chessEngine.optionSpin[multiPV].value = 2
  lib.setOptions(chessEngine)
  eval1 = ffi.new('struct Evaluation *')
  eval2 = ffi.new('struct Evaluation *')
  eval1.maxPlies = 16
  eval2.maxPlies = 16
#  res = lib.engine(engineName, chessEngine, [eval1, eval2])
  lib.isReady()
  lib.newGame()
  lib.position(chessEngine.position, chessEngine.moves)
  lib.go(chessEngine, [eval1, eval2])
  print(f' best move {ffi.string(eval1.bestmove)}, NAG {eval1.nag}', file=sys.stderr)
  lib.quit()

