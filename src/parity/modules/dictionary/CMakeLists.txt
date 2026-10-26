target_sources(lexica_parity PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/module.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/normalize.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/roots.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/seed.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/engine.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/import.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/bootstrap.cpp")
target_link_libraries(lexica_parity PUBLIC winhttp normaliz)
if(BUILD_TESTING)
  add_executable(parity_dictionary_test "${PROJECT_SOURCE_DIR}/tests/parity/dictionary/dictionary_test.cpp")
  target_link_libraries(parity_dictionary_test PRIVATE lexica_parity ws2_32)
  target_include_directories(parity_dictionary_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(parity_dictionary_test PRIVATE
    LEXICA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    LEXICA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    LEXICA_TEST_OUTPUT_DIR="${PROJECT_BINARY_DIR}/test-output")
  add_test(NAME parity_dictionary_test COMMAND parity_dictionary_test)
  set_tests_properties(parity_dictionary_test PROPERTIES TIMEOUT 900)
endif()
