cmake_minimum_required(VERSION 3.21)
# 编译 C 的 SQLite 和 C++ 的业务代码。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LEXICA_VERSION LIMIT_COUNT 1)
project(lexica VERSION ${LEXICA_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Ship a portable Windows executable without a separate Visual C++ runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(NOMINMAX)

if(MINGW)
  add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra)
elseif(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
endif()

# 先把 SQLite 编译成公共库，词库、个人词汇和复习模块都连接它。
add_library(lexica_sqlite third_party/sqlite3.c)
target_include_directories(lexica_sqlite PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(lexica_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_ENABLE_FTS5)
if(MSVC)
  target_compile_options(lexica_sqlite PRIVATE /w)
else()
  target_compile_options(lexica_sqlite PRIVATE -O2 -w)
endif()

# Phase 0 is isolated from the legacy App and database. Production is C++/WebView2;
# the loopback adapter and stdio use the identical router for development/tests.
include(CTest)
add_subdirectory(src/parity)

# 公共库提供错误说明和时间，其他模块在此基础上构建。
add_library(lexica_common
  src/common/error.cpp
  src/common/clock.cpp)
target_include_directories(lexica_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 词库查询、导入和数据库迁移。
add_library(lexica_dict src/dict/dict.cpp)
target_link_libraries(lexica_dict PUBLIC lexica_common lexica_sqlite)

# 个人词汇和活动统计。
add_library(lexica_vocab src/vocab/vocab.cpp)
target_link_libraries(lexica_vocab PUBLIC lexica_common lexica_sqlite)

# 复习出题、调度和成绩。
add_library(lexica_review src/review/review.cpp)
target_link_libraries(lexica_review PUBLIC lexica_dict lexica_vocab lexica_sqlite)

# AI 能力：使用 Windows WinHTTP 请求服务。
add_library(lexica_ai src/ai/ai.cpp)
target_include_directories(lexica_ai SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(lexica_ai PUBLIC lexica_common winhttp)

# App 串联业务模块，接口层不需要直接操作各模块。
add_library(lexica_app src/app/app.cpp)
target_link_libraries(lexica_app PUBLIC lexica_review lexica_vocab lexica_dict lexica_ai)

# HTTP 服务依赖 App 和 Windows 网络库。
add_library(lexica_api src/api/server.cpp)
target_include_directories(lexica_api SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(lexica_api PUBLIC lexica_app ws2_32)

# 把启动入口和控制台界面连接成最终可执行程序。
add_executable(lexica src/main.cpp src/cli/cli.cpp)
target_link_libraries(lexica PRIVATE lexica_api)
if(MINGW)
  target_link_options(lexica PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

# 编译结束后把前端网页复制到 exe 旁边的 web 目录。
add_custom_command(TARGET lexica POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:lexica>/web)

# 默认构建测试；只需要程序时可配置 -DBUILD_TESTING=OFF。
include(CTest)
if(BUILD_TESTING)
  add_executable(dict_test tests/dict_test.cpp)
  target_link_libraries(dict_test PRIVATE lexica_dict)
  add_executable(vocab_tests tests/vocab_tests.cpp)
  target_link_libraries(vocab_tests PRIVATE lexica_vocab lexica_dict)
  add_executable(review_test tests/review_test.cpp)
  target_link_libraries(review_test PRIVATE lexica_app)
  add_executable(import_concurrency_test tests/import_concurrency_test.cpp)
  target_link_libraries(import_concurrency_test PRIVATE lexica_app)

  foreach(test_target dict_test vocab_tests review_test import_concurrency_test)
    target_compile_definitions(${test_target} PRIVATE
      LEXICA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
      LEXICA_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output")
    if(MINGW)
      target_link_options(${test_target} PRIVATE -static-libgcc -static-libstdc++ -static)
    endif()
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 120)
  endforeach()
endif()
