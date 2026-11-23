cmake_minimum_required(VERSION 3.14)
project(TicTacToe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()
add_compile_options(-Wall -Wextra)

# Логика без сети (игра, хранилище, SHA-256) — общая для сервера и тестов.
add_library(ttt_core STATIC
  src/game.cpp
  src/storage.cpp
  src/sha256.cpp
)
target_include_directories(ttt_core PUBLIC src)

add_executable(server
  src/main.cpp
  src/network.cpp
  src/protocol.cpp
)
target_link_libraries(server PRIVATE ttt_core)

add_executable(client client/client.cpp)

# ---------- Google Test ----------
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(tests
    tests/test_game.cpp
    tests/test_storage.cpp
  )
  target_link_libraries(tests PRIVATE ttt_core GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(tests)
endif()
