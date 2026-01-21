cmake_minimum_required(VERSION 3.10)
project(ServerInC C)

set(CMAKE_C_STANDARD 11)

add_executable(ServerInC main.c)

if (WIN32)
    target_link_libraries(ServerInC PRIVATE ws2_32)
endif()
