target_sources(lexica_parity PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/module.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/text.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/http.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/keystore.cpp")
target_link_libraries(lexica_parity PUBLIC winhttp advapi32 normaliz)
if(BUILD_TESTING)
  add_executable(parity_ai_test "${PROJECT_SOURCE_DIR}/tests/parity/ai/ai_test.cpp")
  target_link_libraries(parity_ai_test PRIVATE lexica_parity ws2_32)
  target_include_directories(parity_ai_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(parity_ai_test PRIVATE
    LEXICA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    LEXICA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    LEXICA_TEST_OUTPUT_DIR="${PROJECT_BINARY_DIR}/test-output")
  add_test(NAME parity_ai_test COMMAND parity_ai_test)
  set_tests_properties(parity_ai_test PROPERTIES TIMEOUT 120)
endif()
