cmake_minimum_required (VERSION 3.8)
project(libchess)
add_library (chess SHARED "fen.c" "board.c" "bitscanner.c" "dbsearch.c" "square.c" "piece.c" "zobrist-hash.c" "move.c" "game.c" "tag.c" "engine.c")
target_link_libraries(chess pthread sqlite3)
#add_compile_options(-fPIC)
