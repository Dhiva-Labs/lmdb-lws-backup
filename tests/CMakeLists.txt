# Each test file is a standalone executable registered with CTest.
set(UNIT_TESTS
  test_hot_copy
  test_crypto
  test_manifest
  test_retention
  test_config)

foreach(t ${UNIT_TESTS})
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE lwsbk_core)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Integration test drives the real binary as a subprocess.
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE lwsbk_core)
target_include_directories(test_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_integration
         COMMAND test_integration $<TARGET_FILE:lmdb-lws-backup>)
set_tests_properties(test_integration PROPERTIES TIMEOUT 300)
