import sys, os
from datetime import datetime 
from pathlib import Path
from chess import ffi, lib
    
if __name__ == "__main__":
  threads = 1
  sql_threads = 8 # needed only for updateDb
  generateZobristHash = False
  updateDb = False
  createDataset = True
  minElo = 0 #2400
  maxEloDiff = 4000 #200
  minMoves = 0 #40
  engineEval = True
  engineLogging = False
  multiPV = 1
  hashSize = 128
  engineThreads = 1
  depth = 20
  movetime = 0
  #syzygyPath = '/home/apoliakevitch/syzygy'
  syzygyPath = '/Users/ap/syzygy'
  syzygy = ffi.new('char[]', syzygyPath.encode())
  #enginePath = '/home/apoliakevitch/libchess/stockfish'
  enginePath = '/Users/ap/libchess/stockfish-macos-m1-apple-silicon'
  engine = ffi.new('char[]', enginePath.encode())
  ecoFile = 'eco.pgn'
  datasetTrain = 'games-train'
  datasetTest = 'games-test'
  numberOfLines = 1024 * 1024
  numberOfGames = 100000000 #max number of games per thread to play
  pgnDir = '/Users/ap/libchess'
  #pgnDir = '/home/apoliakevitch/pgn'
  #pgnFilesTest = ['KingBaseLite2019-01.pgn', 'KingBaseLite2019-02.pgn', 'KingBaseLite2019-03.pgn']
  #pgnFilesTrain = ['KingBaseLite2019-A00-A39.pgn', 'KingBaseLite2019-A40-A79.pgn', 'KingBaseLite2019-A80-A99.pgn', 'KingBaseLite2019-B00-B19.pgn', 'KingBaseLite2019-B20-B49.pgn', 'KingBaseLite2019-B50-B99.pgn', 'KingBaseLite2019-C00-C19.pgn', 'KingBaseLite2019-C20-C59.pgn', 'KingBaseLite2019-C60-C99.pgn', 'KingBaseLite2019-D00-D29.pgn', 'KingBaseLite2019-D30-D69.pgn', 'KingBaseLite2019-D70-D99.pgn', 'KingBaseLite2019-E00-E19.pgn', 'KingBaseLite2019-E20-E59.pgn', 'KingBaseLite2019-E60-E99.pgn']
  #pgnFilesTrain = ['KingBaseLite2019.pgn']
  dataset = datasetTrain # datasetTrain, datasetTest or None
  #pgnFiles = pgnFilesTrain # pgnFilesTrain or pgnFilesTest
  pgn_dir = Path(pgnDir)
  pgnFilesList = [ffi.new("char[]", (str(s)).encode()) for s in pgn_dir.glob("test.pgn") if s.is_file()]
  fileNames = ffi.new('char*[]', pgnFilesList)
  ecoFileName = ffi.NULL # no ECO classification is needed
  #ecoFileName = ffi.new('char[]', ecoFile.encode())
  ds = ffi.new('char[]', dataset.encode()) # it will be appended by thread number and .csv ext
  #ds = ffi.NULL
  num = 0;
  start = datetime.now()
  res = lib.openGamesFromPGNfiles(fileNames, len(pgnFilesList), threads, sql_threads, ecoFileName, minElo, maxEloDiff, minMoves, numberOfGames, generateZobristHash, updateDb, createDataset, ds, engineEval, engine, movetime, depth, hashSize, engineThreads, syzygy, multiPV, engineLogging)
  num += res
  if ds != ffi.NULL:
    ffi.release(ds) 
  if ecoFileName != ffi.NULL:
    ffi.release(ecoFileName)
  for s in pgnFilesList:
    ffi.release(s)
  ffi.release(fileNames)
  if engine != ffi.NULL:
    ffi.release(engine)
  if syzygy != ffi.NULL:
    ffi.release(syzygy)

  print(f'Time spent: {datetime.now() - start}')
  print(f' Total games: {num}')

"""
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
  print(f' Total games: {numberOfGames}')
"""
"""
  fileNumber = 0
  lineNumber = 0
  output_file = f'{dataset}-{str(fileNumber)}.pgn' 
  outfile = open(output_file, 'a')
  for t in range(threads):
    input_file = f'{dataset + str(t)}.pgn'
    with open(input_file, 'r') as infile:
      for line in infile:
        if lineNumber < numberOfLines:
          outfile.write(line)
          lineNumber += 1
        else:
          lineNumber = 0
          outfile.close()
          fileNumber += 1
          output_file = f'{dataset}-{str(fileNumber)}.pgn' 
          outfile = open(output_file, 'a')
          outfile.write(line)
          lineNumber += 1
    os.unlink(input_file)
  outfile.close()
  print(f' Total games: {numberOfGames}, output files: {fileNumber}')
 """