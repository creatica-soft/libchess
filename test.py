import sys, time
from chess import ffi, lib
import argparse
parser = argparse.ArgumentParser()
parser.add_argument("pgnfile", help="pgn file to import into next_moves db", type=str)
parser.add_argument("db", help="next_moves sqlie3 db file", type=str)
parser.add_argument("minElo", help="min Elo", type=int)
parser.add_argument("maxEloDiff", help="max Elo difference between opponents", type=int)
args = parser.parse_args()

if __name__ == "__main__":
#  fenString = ffi.new('char[]', b'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1')
#  fenString = ffi.new('char[]', b'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1')
#  f = ffi.new('struct Fen *')
#  board = ffi.new('struct Board *')
#  lib.strtofen(f, fenString)
#  lib.fentostr(f)
#  print(f' init fen: {ffi.string(fenString)}, fen: {ffi.string(f.fenString)}')
#  lib.fentoboard(f, board)
#  for i in range(16):
#    print(f' board->occupations[{i}]: {board.occupations[i]}')
#  for mv in [b'e4', b'e5', b'Nf3', b'Nc6']:
#    move = ffi.new('struct Move *')
#    m = ffi.new('char[]', mv)
#    lib.initMove(move, board, m)
#  print(f' san move {ffi.string(move.sanMove)} uci move {ffi.string(move.uciMove)}')
#    lib.makeMove(move)
#    print(f' fen after move {ffi.string(m)}: {ffi.string(move.chessBoard.fen.fenString)}')

#  fileName = ffi.new('char[]', b'KingBaseLite2019-E60-E99.pgn')

#  fileName = ffi.new('char[]', b'test.pgn')
#  ecoFileName = ffi.new('char[]', b'eco.pgn')
#  engineName = ffi.new('char[]', b'stockfish')
#  chessEngine = ffi.new('struct Engine *')
#  chessEngine.position = b''
#  chessEngine.moves = b'e2e4'
#  chessEngine.movetime = 5000
#  chessEngine.depth = 20
#  chessEngine.nodes = 0
#  chessEngine.searchmoves = ffi.NULL
#  chessEngine.infinite = False
#  chessEngine.ponder = False
#  chessEngine.mate = 0
#  chessEngine.movestogo = 0
#  chessEngine.wtime = 0
#  chessEngine.btime = 0
#  chessEngine.winc = 0
#  chessEngine.binc = 0
#  res = lib.getOptions(engineName, chessEngine)
#  MultiPV = ffi.new('char[]', b'MultiPV')
#  Spin = ffi.cast('enum OptionType', lib.Spin)
#  multiPV = lib.nametoindex(chessEngine, MultiPV, lib.Spin)
#  chessEngine.optionSpin[multiPV].value = 2
#  eval1 = ffi.new('struct Evaluation *')
#  eval2 = ffi.new('struct Evaluation *')
#  eval1.maxPlies = 16
#  eval2.maxPlies = 16
#  res = lib.engine(engineName, chessEngine, [eval1, eval2])
#  print(f' engine returned {res}, best move {ffi.string(eval1.bestmove)}, NAG {eval1.nag}', file=sys.stderr)
  print(f' {time.asctime()}')
#  res = lib.pgnGames(fileName, ffi.NULL, True)
  res = lib.pgnGames(ffi.from_buffer(bytes(args.pgnfile, "ascii")), ffi.NULL, ffi.from_buffer(bytes(args.db, "ascii")), True, args.minElo, args.maxEloDiff)
  print(f' {time.asctime()}')
  print(f' pgnGames return {res}')
#  ffi.release(fileName)

#  fileName = ffi.new('char[]', b'test.pgn')
#  gameStartPositions = ffi.new('unsigned long[256000]')
#  res = lib.countGames(fileName, gameStartPositions, 256000)
#  print(f' countGames {res}')
#  tags = ffi.new('struct Tag[16]')
#  res = lib.gameTags(tags, 16, fileName, gameStartPositions, 0)
#  print(f' gameTags {res}')
#  for tag in tags:
#    print(f' {tag.name} {ffi.string(tag.value)}')
#  for g in range(0,res):
#    if g % 1000 == 0:
#      print(f' game {g} ')
#    game = ffi.new('struct Game *')
#    res = lib.pgnGame(game, fileName, gameStartPositions, g, tags, 16, False, False)
#    ffi.release(game)
#    if (res != 0):
#      break
#  for sq in range(0,64):
#    moves = lib.moveGenerator(lib.Knight, sq)
#  print(f' {lib.R} on {sq} moves {moves}')
