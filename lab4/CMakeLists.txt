cmake_minimum_required(VERSION 3.31)
project(4)

set(CMAKE_CXX_STANDARD 20)

add_executable(4 main.cpp
        server.cpp
        client.cpp)
