include(GoogleTest)

# Every gtest case becomes its own ctest entry, so `ctest -N` reports the real
# number of assertions the suite covers.
function(nvmess_add_gtest name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE nvmeslotswap_core GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    gtest_discover_tests(${name} PROPERTIES TIMEOUT 300)
endfunction()

nvmess_add_gtest(test_sha256)
nvmess_add_gtest(test_queue)
nvmess_add_gtest(test_device)
nvmess_add_gtest(test_fw)
nvmess_add_gtest(test_concurrency)
nvmess_add_gtest(test_swap_integrity)
