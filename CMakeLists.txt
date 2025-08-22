cmake_minimum_required(VERSION 3.10)
project(chess LANGUAGES C CXX)

# Debug messages
message(STATUS "--- COMPILER CHECK ---")
message(STATUS "C Compiler ID: ${CMAKE_C_COMPILER_ID}")
message(STATUS "CXX Compiler ID: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "MSVC variable is: ${MSVC}")
message(STATUS "----------------------")

# Enable C and C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the library (libchess.dll or chess.lib)
add_library(chess SHARED
    bitscanner.c
    board.c
    engine.cpp
    fen.c
    move.c
    piece.c
    square.c
    tag.c
    zobrist-hash.c
    magic_bitboards.c
    nnue/nnue/network.cpp
    nnue/nnue/nnue_accumulator.cpp
    nnue/nnue/nnue_misc.cpp
    nnue/nnue/features/half_ka_v2_hm.cpp
    nnue/bitboard.cpp
    nnue/evaluate.cpp
    nnue/memory.cpp
    nnue/misc.cpp
    nnue/nnue.cpp
    nnue/position.cpp
)

# Define the executable
add_executable(chess_mcts_smp
    chess_mcts_smp.cpp
    uci_smp.cpp
    tbcore.c
    tbprobe.c
)

# Now set compiler options after targets are defined
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl specific options
    message(STATUS "clang-cl compiler detected. Using Clang-style flags.")
    
    target_compile_options(chess PRIVATE
        -W3
        -D_CRT_SECURE_NO_WARNINGS
        -Wno-deprecated-declarations
        -Wno-conversion
        -Wno-writable-strings
        -Wno-unused-variable
        -Wno-unused-function
    )
    
    target_compile_options(chess_mcts_smp PRIVATE
        -W3
        -D_CRT_SECURE_NO_WARNINGS
        -Wno-deprecated-declarations
        -Wno-conversion
        -Wno-writable-strings
        -Wno-unused-variable
        -Wno-unused-function
    )
    
    # Release-specific optimizations
    target_compile_options(chess PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(chess_mcts_smp PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    
    # Linker flags for LTO
    set_target_properties(chess chess_mcts_smp PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )

elseif(MSVC)
    # MSVC specific options
    message(STATUS "MSVC compiler detected. Using MSVC-style flags.")

    target_compile_options(chess PRIVATE
        /utf-8
        /MP
        /W3
        /D_CRT_SECURE_NO_WARNINGS
        /wd4101
        /wd4996
        /wd4267
        /wd4244
        /wd4334
        /wd4022
        /wd4312
    )
    
    target_compile_options(chess_mcts_smp PRIVATE
        /utf-8
        /MP
        /W3
        /D_CRT_SECURE_NO_WARNINGS
        /wd4101
        /wd4996
        /wd4267
        /wd4244
        /wd4334
        /wd4022
        /wd4312
    )
    
    # Release-specific optimizations
    target_compile_options(chess PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(chess_mcts_smp PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    
    # Enable LTO
    set_target_properties(chess chess_mcts_smp PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )

else()
    # GCC/Clang on non-Windows
    message(STATUS "Non-MSVC Clang/GCC compiler detected.")
    
    target_compile_options(chess PRIVATE $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>)
    target_compile_options(chess_mcts_smp PRIVATE $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>)
endif()

# Include directories
target_include_directories(chess PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
target_include_directories(chess_mcts_smp PRIVATE ${PROJECT_SOURCE_DIR}/nnue)

# Link executable with libchess
target_link_libraries(chess_mcts_smp PRIVATE chess)