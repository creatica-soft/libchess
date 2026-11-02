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
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the library (libchess.dll or chess.lib)
set(CHESS_SOURCES board.cpp engine.cpp fen.cpp move.cpp pgn.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp)
add_library(chess SHARED ${CHESS_SOURCES})

# Define the executable
# The historical MCTS engine, kept as a target so it cannot rot unnoticed. Shares uci_frontend.cpp
# on purpose -- see the note at the top of creatica_search_mcts.cpp.
add_executable(creatica_mcts creatica_search_mcts.cpp creatica_mcts.cpp uci_frontend.cpp tbprobe.c)
add_executable(creatica_ab creatica_ab.cpp uci_frontend.cpp tbprobe.c)
# The ablation copy of creatica_ab, which stays the baseline; see the top of creatica_abl.cpp.
add_executable(creatica_abl creatica_abl.cpp uci_frontend.cpp tbprobe.c)
# Alpha-beta from first principles, built up in measured layers; see the top of creatica_fp.cpp.
add_executable(creatica_fp creatica_fp.cpp uci_frontend.cpp tbprobe.c)
# Component A of the chess GUI. A CMake target because it was NOT one: the binary went stale on
# 2025-09-15 and nothing rebuilt it, so when the Board struct lost its cache fields the stale
# executable kept the old layout inlined while linking the new library. Every field offset past
# the removed ones was wrong, and the tournament died on an lsBit(0) assertion with a perfectly
# legal opening position. A copy that is not built is a copy that rots unnoticed.
add_executable(gui_helper gui_helper.cpp)
add_executable(test_pos test_pos.cpp)
# The regression guard for get_see(); see the note at the top of test_see.cpp.
add_executable(test_see test_see.cpp)
add_executable(perft perft.cpp)
add_executable(test_nnue tbprobe.c test_nnue.cpp)
add_executable(test_tb tbprobe.c test_tb.cpp)
add_executable(test_smp tbprobe.c test_smp.cpp)
add_executable(test_bm test_bm.cpp)
add_executable(tournament tournament.cpp)
add_executable(lichess_bot lichess_bot.cpp)

# Now set compiler options after targets are defined
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl specific options
    message(STATUS "clang-cl compiler detected. Using Clang-style flags.")
    
    target_compile_options(chess PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_tb PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_nnue PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_smp PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_pos PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_bm PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(tournament PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(lichess_bot PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS)
    
    # Release-specific optimizations
    target_compile_options(chess PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(creatica_mcts PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(creatica_ab PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(creatica_abl PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_tb PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_nnue PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_smp PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_pos PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_bm PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(tournament PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(lichess_bot PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    
    # Linker flags for LTO
    set_target_properties(chess test_pos test_tb test_nnue test_smp tournament lichess_bot PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

elseif(MSVC)
    # MSVC specific options
    message(STATUS "MSVC compiler detected. Using MSVC-style flags.")

    target_compile_options(chess PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(creatica_mcts PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(creatica_ab PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(creatica_abl PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_nnue PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_tb PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_smp PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_pos PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_bm PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(tournament PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(lichess_bot PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    
    # Release-specific optimizations
    target_compile_options(chess PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(creatica_mcts PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(creatica_ab PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(creatica_abl PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_nnue PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_tb PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_smp PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_pos PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_bm PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(tournament PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(lichess_bot PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    
    # Enable LTO
    set_target_properties(chess test_nnue test_tb test_smp test_pos tournament lichess_bot PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

else()
    # GCC/Clang on non-Windows
    message(STATUS "Non-MSVC Clang/GCC compiler detected.")
    # The rpath is the SOURCE DIRECTORY, because the build is in-source: the generated Makefile,
    # CMakeCache.txt and libchess.dylib all land in the repo root, so this is where the executables
    # must look for the library at run time. It used to be the absolute path of one machine's
    # checkout, which both leaked a home directory into a public repository and silently produced
    # executables that could not find libchess elsewhere.
    set(CMAKE_INSTALL_RPATH "${CMAKE_CURRENT_SOURCE_DIR}")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

    target_link_options(chess PRIVATE -Wl,-rpath,${CMAKE_CURRENT_SOURCE_DIR})
    
    # -Wall rather than a list of -Wno-*: every source below is clean under it.
    # tbcore.c is deliberately not a target source -- tbprobe.c #includes it, and the
    # separate object exported nothing while producing a dozen unused-symbol warnings.
    target_compile_options(chess PRIVATE -Wall -O3 -march=native -DNDEBUG)
    # creatica_ab and creatica_mcts are built as ONE binary each, with the library's sources compiled
    # in, so full LTO can optimise across the search/library boundary, as Stockfish's own build does.
    # On Apple Silicon they take Stockfish's apple-silicon flags (Makefile: ARCH=apple-silicon
    # COMP=clang), NEON and dot-product kernels included. libchess.dylib keeps its own flags: the
    # other targets load it.
    function(stockfish_flags tgt)
        if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            target_compile_options(${tgt} PRIVATE -Wall -DNDEBUG -DUSE_PTHREADS -O3 -funroll-loops
                -mdynamic-no-pic -DIS_64BIT -DUSE_POPCNT -DUSE_NEON=8 -march=armv8.2-a+dotprod
                -DUSE_NEON_DOTPROD -flto=full)
            target_link_options(${tgt} PRIVATE -flto=full)
        else()
            target_compile_options(${tgt} PRIVATE -Wall -O3 -march=native -DNDEBUG -flto)
            target_link_options(${tgt} PRIVATE -flto)
        endif()
    endfunction()
    function(build_like_stockfish tgt)
        target_sources(${tgt} PRIVATE ${CHESS_SOURCES})
        stockfish_flags(${tgt})
    endfunction()
    build_like_stockfish(creatica_ab)
    build_like_stockfish(creatica_abl)
    build_like_stockfish(creatica_fp)
    build_like_stockfish(creatica_mcts)
    target_compile_options(gui_helper PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(test_see PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(test_nnue PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(test_tb PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(test_smp PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(test_pos PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(test_bm PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(perft PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(tournament PRIVATE -Wall -O3 -march=native -DNDEBUG)
    target_compile_options(lichess_bot PRIVATE -Wall -O3 -march=native -DNDEBUG)
endif()

# Include directories
# .incbin is an assembler directive, so CMake cannot see that network.cpp depends on the
# two net files - swapping a net changed nothing until this TU happened to be recompiled,
# silently leaving the old net embedded in the library.
set_source_files_properties(nnue/nnue/network.cpp PROPERTIES OBJECT_DEPENDS
  "${PROJECT_SOURCE_DIR}/nn-c288c895ea92.nnue;${PROJECT_SOURCE_DIR}/nn-37f18f62d772.nnue")

target_include_directories(chess PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
target_include_directories(creatica_mcts PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
target_include_directories(creatica_ab PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
target_include_directories(creatica_abl PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
target_include_directories(creatica_fp PRIVATE ${PROJECT_SOURCE_DIR}/nnue)

# Link executable with libchess
# curl for the online 7-piece tablebase probe. Outside MSVC creatica_ab and creatica_mcts compile
# the library's sources in (build_like_stockfish above) and do not link libchess.
if(MSVC)
    target_link_libraries(creatica_ab PRIVATE chess curl)
    target_link_libraries(creatica_abl PRIVATE chess curl)
    target_link_libraries(creatica_fp PRIVATE chess curl)
    target_link_libraries(creatica_mcts PRIVATE chess curl)
else()
    target_link_libraries(creatica_ab PRIVATE curl pthread)
    target_link_libraries(creatica_abl PRIVATE curl pthread)
    target_link_libraries(creatica_fp PRIVATE curl pthread)
    target_link_libraries(creatica_mcts PRIVATE curl pthread)
endif()
target_link_libraries(gui_helper PRIVATE chess)
target_link_libraries(test_pos PRIVATE chess)
target_link_libraries(test_bm PRIVATE chess)
target_link_libraries(test_see PRIVATE chess)
target_link_libraries(perft PRIVATE chess)
target_link_libraries(test_nnue PRIVATE chess)
target_link_libraries(test_tb PRIVATE chess)
target_link_libraries(test_smp PRIVATE chess)
target_link_libraries(tournament PRIVATE chess)
target_link_libraries(lichess_bot PRIVATE chess curl)

# creatica_ab_pgo: creatica_ab with clang's profile-guided optimisation, the way Stockfish's own
# releases are built. Same search, node for node, about 4% faster. Not part of `make all`, because it
# has to play a training run; ask for it with `make creatica_ab_pgo`. Plain creatica_ab is untouched.
# Changing any source retrains: creatica_ab_gen rebuilds, so the profile does, so the stamp does, and
# every object includes the stamp, which is what recompiles them against the new profile.
if(NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(PGO_DIR ${CMAKE_BINARY_DIR}/CMakeFiles/pgo)
    set(PGO_DATA ${PGO_DIR}/creatica_ab.profdata)
    set(PGO_STAMP ${PGO_DIR}/profile_stamp.h)
    set(PGO_TRAIN ${PROJECT_SOURCE_DIR}/testsets/pgo_train.tsv)
    if(APPLE)
        set(LLVM_PROFDATA xcrun llvm-profdata)
    else()
        find_program(LLVM_PROFDATA llvm-profdata)
    endif()

    add_executable(creatica_ab_gen EXCLUDE_FROM_ALL creatica_ab.cpp uci_frontend.cpp tbprobe.c)
    build_like_stockfish(creatica_ab_gen)
    target_compile_options(creatica_ab_gen PRIVATE -fprofile-instr-generate)
    target_link_options(creatica_ab_gen PRIVATE -fprofile-instr-generate)
    set_target_properties(creatica_ab_gen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PGO_DIR})

    # One thread at fixed nodes: the same training run gives the same profile.
    add_custom_command(OUTPUT ${PGO_DATA} ${PGO_STAMP}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${PGO_DIR}/train.profraw
        COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${PGO_DIR}/train.profraw HOME=/nonexistent
                ENGINE=$<TARGET_FILE:creatica_ab_gen> HASH=64 THREADS=1 MOVETIME=0 NODES=1000000
                $<TARGET_FILE:test_bm> ${PGO_TRAIN} > ${PGO_DIR}/train.out
        COMMAND ${LLVM_PROFDATA} merge -o ${PGO_DATA} ${PGO_DIR}/train.profraw
        COMMAND ${CMAKE_COMMAND} -E touch ${PGO_STAMP}
        DEPENDS creatica_ab_gen test_bm ${PGO_TRAIN}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Training creatica_ab's profile"
        VERBATIM)
    add_custom_target(creatica_ab_profile DEPENDS ${PGO_DATA} ${PGO_STAMP})

    # The training run never enters these files, so they have no profile to use and clang rightly
    # warns if asked to. They are compiled without it and still join the same link-time optimisation.
    set(PGO_UNPROFILED engine.cpp pgn.cpp tag.cpp nnue/nnue/nnue_misc.cpp nnue/memory.cpp nnue/misc.cpp)
    set(PGO_PROFILED ${CHESS_SOURCES})
    list(REMOVE_ITEM PGO_PROFILED ${PGO_UNPROFILED})
    add_library(creatica_ab_unprofiled OBJECT EXCLUDE_FROM_ALL ${PGO_UNPROFILED})
    stockfish_flags(creatica_ab_unprofiled)
    target_include_directories(creatica_ab_unprofiled PRIVATE ${PROJECT_SOURCE_DIR}/nnue)

    add_executable(creatica_ab_pgo EXCLUDE_FROM_ALL creatica_ab.cpp uci_frontend.cpp tbprobe.c
        ${PGO_PROFILED} $<TARGET_OBJECTS:creatica_ab_unprofiled>)
    add_dependencies(creatica_ab_pgo creatica_ab_profile)
    stockfish_flags(creatica_ab_pgo)
    target_compile_options(creatica_ab_pgo PRIVATE -fprofile-instr-use=${PGO_DATA} -include ${PGO_STAMP})
    target_link_options(creatica_ab_pgo PRIVATE -fprofile-instr-use=${PGO_DATA})

    target_include_directories(creatica_ab_gen PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
    target_include_directories(creatica_ab_pgo PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
    target_link_libraries(creatica_ab_gen PRIVATE curl pthread)
    target_link_libraries(creatica_ab_pgo PRIVATE curl pthread)
endif()
