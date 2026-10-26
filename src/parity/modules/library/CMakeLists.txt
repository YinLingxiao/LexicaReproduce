target_sources(lexica_parity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module.cpp")

if(BUILD_TESTING)
  add_executable(parity_library_test "${PROJECT_SOURCE_DIR}/tests/parity/library/library_test.cpp")
  target_link_libraries(parity_library_test PRIVATE lexica_parity)
  target_include_directories(parity_library_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(parity_library_test PRIVATE
    LEXICA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    LEXICA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    LEXICA_TEST_OUTPUT_DIR="${PROJECT_BINARY_DIR}/test-output")
  add_test(NAME parity_library_test COMMAND parity_library_test)
  set_tests_properties(parity_library_test PROPERTIES TIMEOUT 120)
endif()
