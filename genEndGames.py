import sys, random, os
from datetime import datetime
from chess import ffi, lib

if __name__ == "__main__":

  maxGameNumber = 1250 # for each thread (it takes about 5 hours for 1200 x 8 with movetime=5000 and depth 30)
  maxPieceNumber = 5 # 5 is max for now (for 5-piece Syzygy Tables)
  datasetTrain = 'end-games-train'
  datasetTest = 'end-games-test'
  dataset = datasetTrain # datasetTrain, datasetTest or None
  ds = ffi.new('char[]', dataset.encode()) # it will be appended by thread number and .csv ext
  engine = ffi.new('char[]', b'/Users/ap/libchess/stockfish-macos-m1-apple-silicon')
  movetime = 5000 # ms - this affects the speed, faster with less time
  depth = 30 # in halfmoves - affects the speed, faster with shallower depth
  hashSize = 64 # in MB
  threadNumber = 1 # chess engine number of threads
  syzygyPath = ffi.new('char[]', b'/Users/ap/libchess/syzygy/')
  multiPV = 1 #number of variations provided by chess engine
  writedebug = False
  threads = 8 # end game generation number of threads
  
  start = datetime.now()
  lib.generateEndGames(maxGameNumber, maxPieceNumber, ds, engine, movetime, depth, hashSize, threadNumber, syzygyPath, multiPV, writedebug, threads)  
  print(f'Time spent: {datetime.now() - start}')
  
  output_file = f'{dataset}' + '.pgn'
  with open(output_file, 'a') as outfile:
    for t in range(threads):
      input_file = f'{dataset + str(t)}' + '.pgn'
      with open(input_file, 'r') as infile:
        while True:
          chunk = infile.read(1024 * 1024)  # 1MB chunks
          if not chunk:
            break
          outfile.write(chunk)
      os.unlink(input_file)
      
  ffi.release(ds)
  ffi.release(engine)
  ffi.release(syzygyPath)