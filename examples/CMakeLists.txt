foreach(ex 01_files_and_mmap 02_crash_safe_kv 03_concurrent_readers 04_bring_your_own 05_iterate_and_query 06_in_memory)
    add_executable(llbt-example-${ex} ${ex}.cpp)
    target_link_libraries(llbt-example-${ex} PRIVATE llbt::core)
endforeach()
