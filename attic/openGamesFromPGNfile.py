import sys, os
from datetime import datetime 
from chess import ffi, lib
    
if __name__ == "__main__":
  threads = 8
  sql_threads = 0 # needed only if updateDb is True
  generateZobristHash = False
  updateDb = False
  createDataset = True # creates a dataset in CSV format: FEN,UCI move,Game result for training chessCNN model
  minElo = 2400
  maxEloDiff = 200
  ecoFile = "eco.pgn"
  datasetTrain = 'games-train'
  datasetTest = 'games-test'
  dataset = datasetTrain # datasetTrain, datasetTest or None
  pgnFile = 'KingBaseLite2019-01.pgn'
  ecoFileName = ffi.NULL # no ECO classification is needed
  #ecoFileName = ffi.new('char[]', ecoFile.encode())
  ds = ffi.new('char[]', dataset.encode()) # it will be appended by thread number and .csv ext
  fileName = ffi.new('char[]', pgnFile.encode()) 
  numberOfGames = 0;
  start = datetime.now()
  res = lib.openGamesFromPGNfile(fileName, threads, sql_threads, ecoFileName, minElo, maxEloDiff, generateZobristHash, updateDb, createDataset, ds)
  numberOfGames += res
  ffi.release(fileName)
  ffi.release(ds)
  if ecoFileName != ffi.NULL:
    ffi.release(ecoFileName)

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
  print(f' Total games: {numberOfGames}')
