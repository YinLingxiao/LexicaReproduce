cmake_minimum_required(VERSION 3.16)
project(lexica LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MINGW)
  add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra)
endif()

add_library(lexica_common
  src/common/error.cpp
  src/common/clock.cpp)
add_library(lexica_dict src/dict/dict.cpp)
add_library(lexica_vocab src/vocab/vocab.cpp)
add_library(lexica_review src/review/review.cpp)
add_library(lexica_ai src/ai/ai.cpp)
add_library(lexica_app src/app/app.cpp)
add_library(lexica_api src/api/server.cpp)

foreach(target lexica_common lexica_dict lexica_vocab lexica_review lexica_ai lexica_app lexica_api)
  target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/src)
endforeach()

target_link_libraries(lexica_dict PUBLIC lexica_common)
target_link_libraries(lexica_vocab PUBLIC lexica_common)
target_link_libraries(lexica_review PUBLIC lexica_dict lexica_vocab lexica_common)
target_link_libraries(lexica_ai PUBLIC lexica_common)
target_link_libraries(lexica_app PUBLIC lexica_review lexica_vocab lexica_dict lexica_ai)

target_include_directories(lexica_api SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(lexica_api PUBLIC lexica_app ws2_32)

add_executable(lexica src/main.cpp src/cli/cli.cpp)
target_link_libraries(lexica PRIVATE lexica_api)
if(MINGW)
  target_link_options(lexica PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

add_custom_command(TARGET lexica POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:lexica>/web)
