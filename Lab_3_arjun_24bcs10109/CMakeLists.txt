cmake_minimum_required(VERSION 3.14)
project(db_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(db_engine main.cpp)
target_link_libraries(db_engine PRIVATE Threads::Threads)
target_compile_options(db_engine PRIVATE -Wall -Wextra -Wpedantic)
