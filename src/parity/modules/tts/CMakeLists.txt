target_sources(lexica_parity PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/engine.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/module.cpp")

set(LEXICA_TTS_WINRT 0)
if(MSVC AND CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
  set(LEXICA_CPPWINRT_HEADER "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt/winrt/Windows.Media.SpeechSynthesis.h")
  if(EXISTS "${LEXICA_CPPWINRT_HEADER}")
    target_sources(lexica_parity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/winrt_engine.cpp")
    target_link_libraries(lexica_parity PUBLIC runtimeobject sapi)
    set(LEXICA_TTS_WINRT 1)
  endif()
endif()
target_compile_definitions(lexica_parity PRIVATE LEXICA_TTS_WINRT=${LEXICA_TTS_WINRT})

if(BUILD_TESTING)
  add_executable(parity_tts_test "${PROJECT_SOURCE_DIR}/tests/parity/tts/tts_test.cpp")
  target_link_libraries(parity_tts_test PRIVATE lexica_parity)
  target_include_directories(parity_tts_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(parity_tts_test PRIVATE
    LEXICA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    LEXICA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    LEXICA_TEST_OUTPUT_DIR="${PROJECT_BINARY_DIR}/test-output")
  add_test(NAME parity_tts_test COMMAND parity_tts_test)
  set_tests_properties(parity_tts_test PROPERTIES TIMEOUT 180)
endif()
