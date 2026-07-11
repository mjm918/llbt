foreach(ex 01_files_and_mmap 02_crash_safe_kv 03_concurrent_readers 04_bring_your_own 05_iterate_and_query 06_in_memory)
    add_executable(llbt-example-${ex} ${ex}.cpp)
    target_link_libraries(llbt-example-${ex} PRIVATE llbt::core)
endforeach()

# Compiled as C (proving <llbt/c_api.h> is valid C) but linked with the C++
# driver so the C++ core's runtime comes along.
add_executable(llbt-example-07_c_api 07_c_api.c)
target_link_libraries(llbt-example-07_c_api PRIVATE llbt::core)
set_target_properties(llbt-example-07_c_api PROPERTIES LINKER_LANGUAGE CXX)
