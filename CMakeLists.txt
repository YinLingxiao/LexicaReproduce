cmake_minimum_required(VERSION 3.16)
project(lexica LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MINGW)
  add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra)
endif()

add_library(lexica_common
  a/common/error.cpp
  a/common/clock.cpp)
target_include_directories(lexica_common PUBLIC ${CMAKE_SOURCE_DIR}/a)

add_library(lexica_dict c/dict/dict.cpp)
target_include_directories(lexica_dict PUBLIC ${CMAKE_SOURCE_DIR}/c)
target_link_libraries(lexica_dict PUBLIC lexica_common)

add_library(lexica_vocab d/vocab/vocab.cpp)
target_include_directories(lexica_vocab PUBLIC ${CMAKE_SOURCE_DIR}/d)
target_link_libraries(lexica_vocab PUBLIC lexica_common)

add_library(lexica_review e/review/review.cpp)
target_include_directories(lexica_review PUBLIC ${CMAKE_SOURCE_DIR}/e)
target_link_libraries(lexica_review PUBLIC lexica_dict lexica_vocab)

add_library(lexica_ai a/ai/ai.cpp)
target_link_libraries(lexica_ai PUBLIC lexica_common)

add_library(lexica_app a/app/app.cpp)
target_link_libraries(lexica_app PUBLIC lexica_review lexica_vocab lexica_dict lexica_ai)

add_library(lexica_api a/api/server.cpp)
target_include_directories(lexica_api SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(lexica_api PUBLIC lexica_app ws2_32)

add_executable(lexica a/main.cpp a/cli/cli.cpp)
target_link_libraries(lexica PRIVATE lexica_api)
if(MINGW)
  target_link_options(lexica PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

add_custom_command(TARGET lexica POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/b/web $<TARGET_FILE_DIR:lexica>/web)
