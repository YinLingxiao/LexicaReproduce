set(LEXICA_MIGRATIONS "")
file(GLOB migration_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/storage/migrations/*.sql")
list(SORT migration_files)
foreach(migration IN LISTS migration_files)
  file(READ "${migration}" migration_sql)
  string(APPEND LEXICA_MIGRATIONS "R\"SQL(${migration_sql})SQL\",\n")
endforeach()
file(READ "${PROJECT_SOURCE_DIR}/resources/seed.json" seed_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," LEXICA_SEED_BYTES "${seed_hex}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(storage/embedded.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/parity_resources.h" @ONLY)

add_library(lexica_parity
  contracts/clock.cpp contracts/cancel.cpp storage/database.cpp storage/legacy.cpp
  storage/upstream.cpp api/router.cpp api/events.cpp app/application.cpp app/paths.cpp
  app/executor.cpp)
target_include_directories(lexica_parity PUBLIC "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/third_party"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(lexica_parity PUBLIC lexica_sqlite)
foreach(domain dictionary learning library ai tts)
  # Each agent owns its local CMakeLists and source list; no root build edits.
  add_subdirectory("modules/${domain}")
endforeach()

add_executable(lexica_parity_server api/main.cpp)
target_link_libraries(lexica_parity_server PRIVATE lexica_parity ws2_32)
target_compile_definitions(lexica_parity_server PRIVATE LEXICA_DEFAULT_UI_DIR="${PROJECT_SOURCE_DIR}/ui/build")

option(LEXICA_BUILD_DESKTOP "Build the MSVC Win32/WebView2 host" ${MSVC})
set(LEXICA_WEBVIEW2_ROOT "${PROJECT_SOURCE_DIR}/.deps/webview2/1.0.4314.35" CACHE PATH "Pinned WebView2 SDK root")
if(LEXICA_BUILD_DESKTOP)
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Desktop baseline requires MSVC x64. Use parity-msvc or disable LEXICA_BUILD_DESKTOP.")
  endif()
  if(NOT EXISTS "${LEXICA_WEBVIEW2_ROOT}/build/native/include/WebView2.h")
    message(FATAL_ERROR "Run tools/bootstrap.ps1 to download the pinned WebView2 SDK.")
  endif()
  add_executable(lexica_desktop WIN32 desktop/main.cpp)
  target_include_directories(lexica_desktop PRIVATE "${LEXICA_WEBVIEW2_ROOT}/build/native/include")
  add_custom_command(TARGET lexica_desktop POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${PROJECT_SOURCE_DIR}/resources/lexica.ico"
      "$<TARGET_FILE_DIR:lexica_desktop>/lexica.ico"
    VERBATIM)
  target_link_libraries(lexica_desktop PRIVATE lexica_parity
    "${LEXICA_WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib"
    ole32 oleaut32 shlwapi shell32 user32 version advapi32)
endif()

if(MSVC)
  enable_language(RC)
  configure_file(desktop/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc" @ONLY)
  target_sources(lexica_parity_server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
  if(TARGET lexica_desktop)
    target_sources(lexica_desktop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
  endif()
endif()

find_program(LEXICA_NPM NAMES npm.cmd npm HINTS "C:/Program Files/nodejs")
if(LEXICA_NPM)
  add_custom_target(lexica_ui
    COMMAND "${LEXICA_NPM}" run build
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/ui" VERBATIM)
  if(TARGET lexica_desktop)
    add_custom_target(lexica_package
      COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/ui/build" "$<TARGET_FILE_DIR:lexica_desktop>/ui"
      DEPENDS lexica_ui lexica_desktop VERBATIM)
  endif()
endif()

if(BUILD_TESTING)
  add_executable(parity_foundation_test "${PROJECT_SOURCE_DIR}/tests/parity/foundation_test.cpp")
  target_link_libraries(parity_foundation_test PRIVATE lexica_parity)
  target_include_directories(parity_foundation_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(parity_foundation_test PRIVATE
    LEXICA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    LEXICA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    LEXICA_TEST_OUTPUT_DIR="${PROJECT_BINARY_DIR}/test-output")
  add_test(NAME parity_foundation_test COMMAND parity_foundation_test)
  set_tests_properties(parity_foundation_test PROPERTIES TIMEOUT 60)
  add_executable(parity_platform_test "${PROJECT_SOURCE_DIR}/tests/parity/platform_test.cpp")
  target_link_libraries(parity_platform_test PRIVATE lexica_parity)
  target_include_directories(parity_platform_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(parity_platform_test PRIVATE
    LEXICA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    LEXICA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    LEXICA_TEST_OUTPUT_DIR="${PROJECT_BINARY_DIR}/test-output")
  add_test(NAME parity_platform_test COMMAND parity_platform_test)
  set_tests_properties(parity_platform_test PROPERTIES TIMEOUT 120)
endif()
