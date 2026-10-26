target_sources(lexica_parity PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/module.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sql.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/memory_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/memory_service.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/grader.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/review_service.cpp)
if(BUILD_TESTING)
  add_executable(parity_learning_test
    ${PROJECT_SOURCE_DIR}/tests/parity/learning/learning_test.cpp)
  target_link_libraries(parity_learning_test PRIVATE lexica_parity)
  target_include_directories(parity_learning_test PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_compile_definitions(parity_learning_test PRIVATE
    LEXICA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    LEXICA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    LEXICA_TEST_OUTPUT_DIR="${PROJECT_BINARY_DIR}/test-output")
  add_test(NAME parity_learning_test COMMAND parity_learning_test)
  set_tests_properties(parity_learning_test PROPERTIES TIMEOUT 120)
endif()
