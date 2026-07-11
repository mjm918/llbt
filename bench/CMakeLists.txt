add_executable(llbt-bench bench.cpp)
target_link_libraries(llbt-bench PRIVATE llbt::core)

add_executable(llbt-bench-concurrency bench_concurrency.cpp)
target_link_libraries(llbt-bench-concurrency PRIVATE llbt::core)

add_executable(llbt-bench-group-commit bench_group_commit.cpp)
target_link_libraries(llbt-bench-group-commit PRIVATE llbt::core)

# Optional side-by-side comparison vs LMDB (the truest peer engine).
# Install it first: `brew install lmdb` or `apt install liblmdb-dev`.
execute_process(COMMAND brew --prefix lmdb OUTPUT_VARIABLE _brew_lmdb
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
find_path(LMDB_INCLUDE_DIR lmdb.h
    HINTS ${_brew_lmdb}/include PATHS /opt/homebrew/opt/lmdb/include /usr/local/include /usr/include)
find_library(LMDB_LIBRARY NAMES lmdb
    HINTS ${_brew_lmdb}/lib PATHS /opt/homebrew/opt/lmdb/lib /usr/local/lib /usr/lib)

if(LMDB_INCLUDE_DIR AND LMDB_LIBRARY)
    add_executable(llbt-bench-compare bench_compare.cpp)
    target_include_directories(llbt-bench-compare PRIVATE ${LMDB_INCLUDE_DIR})
    target_link_libraries(llbt-bench-compare PRIVATE llbt::core ${LMDB_LIBRARY})
    message(STATUS "llbt bench: LMDB found (${LMDB_LIBRARY}) — building llbt-bench-compare")
else()
    message(STATUS "llbt bench: LMDB not found — skipping llbt-bench-compare (brew install lmdb)")
endif()
