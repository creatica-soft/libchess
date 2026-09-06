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
add_library(chess SHARED board.cpp engine.cpp fen.cpp move.cpp pgn.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp)

# Define the executable
#add_executable(creatica attic/creatica.cpp attic/uci.cpp tbcore.c tbprobe.c)  # historical; does not compile against the current API
add_executable(test_pos test_pos.cpp)
add_executable(perft perft.cpp)
add_executable(test_nnue tbcore.c tbprobe.c test_nnue.cpp)
add_executable(test_tb tbcore.c tbprobe.c test_tb.cpp)
add_executable(test_smp tbcore.c tbprobe.c test_smp.cpp)
add_executable(tournament tournament.cpp)
add_executable(lichess_bot lichess_bot.cpp)

# Now set compiler options after targets are defined
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl specific options
    message(STATUS "clang-cl compiler detected. Using Clang-style flags.")
    
    target_compile_options(chess PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    #target_compile_options(creatica PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_tb PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_nnue PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_smp PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_pos PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(tournament PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(lichess_bot PRIVATE -W3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    
    # Release-specific optimizations
    target_compile_options(chess PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    #target_compile_options(creatica PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_tb PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_nnue PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_smp PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(test_pos PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(tournament PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    target_compile_options(lichess_bot PRIVATE $<$<CONFIG:Release>:-O3 -march=native -ffast-math  -flto -mavx2 -mbmi2 -mpopcnt>)
    
    # Linker flags for LTO
    set_target_properties(chess test_pos test_tb test_nnue test_smp tournament lichess_bot PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

elseif(MSVC)
    # MSVC specific options
    message(STATUS "MSVC compiler detected. Using MSVC-style flags.")

    target_compile_options(chess PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    #target_compile_options(creatica PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_nnue PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_tb PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_smp PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(test_pos PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(tournament PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    target_compile_options(lichess_bot PRIVATE /utf-8 /MP /W3 /D_CRT_SECURE_NO_WARNINGS  /DNDEBUG /DNO_PREFETCH /DUSE_POPCNT /DUSE_PEXT /wd4101 /wd4996 /wd4267 /wd4244 /wd4334 /wd4022 /wd4312)
    
    # Release-specific optimizations
    target_compile_options(chess PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    #target_compile_options(creatica PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_nnue PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_tb PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_smp PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(test_pos PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(tournament PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    target_compile_options(lichess_bot PRIVATE $<$<CONFIG:Release>:/O2 /Ob2 /arch:AVX2 /fp:fast>)
    
    # Enable LTO
    set_target_properties(chess test_nnue test_tb test_smp test_pos tournament lichess_bot PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

else()
    # GCC/Clang on non-Windows
    message(STATUS "Non-MSVC Clang/GCC compiler detected.")
    set(CMAKE_INSTALL_RPATH "/Users/ap/libchess")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

    target_compile_options(chess PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_link_options(chess PRIVATE -Wl,-rpath,/Users/ap/libchess)
    #target_compile_options(creatica PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_nnue PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_tb PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_smp PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(test_pos PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(perft PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(tournament PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    target_compile_options(lichess_bot PRIVATE -Wno-deprecated-declarations -Wno-conversion -Wno-writable-strings -Wno-unused-variable -Wno-unused-function)
    
    target_compile_options(chess PRIVATE -O3 -march=native -DNDEBUG)
    #target_compile_options(creatica PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(test_nnue PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(test_tb PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(test_smp PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(test_pos PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(perft PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(tournament PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(lichess_bot PRIVATE -O3 -march=native -DNDEBUG)
endif()

# Include directories
# .incbin is an assembler directive, so CMake cannot see that network.cpp depends on the
# two net files - swapping a net changed nothing until this TU happened to be recompiled,
# silently leaving the old net embedded in the library.
set_source_files_properties(nnue/nnue/network.cpp PROPERTIES OBJECT_DEPENDS
  "${PROJECT_SOURCE_DIR}/nn-c288c895ea92.nnue;${PROJECT_SOURCE_DIR}/nn-37f18f62d772.nnue")

target_include_directories(chess PRIVATE ${PROJECT_SOURCE_DIR}/nnue)
#target_include_directories(creatica PRIVATE ${PROJECT_SOURCE_DIR}/nnue)

# Link executable with libchess
#target_link_libraries(creatica PRIVATE chess curl)
target_link_libraries(test_pos PRIVATE chess)
target_link_libraries(perft PRIVATE chess)
target_link_libraries(test_nnue PRIVATE chess)
target_link_libraries(test_tb PRIVATE chess)
target_link_libraries(test_smp PRIVATE chess)
target_link_libraries(tournament PRIVATE chess)
target_link_libraries(lichess_bot PRIVATE chess curl)
