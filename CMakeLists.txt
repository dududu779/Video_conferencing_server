cmake_minimum_required(VERSION 3.10)
project(Server C CXX)
set(CMAKE_CXX_COMPILER g++)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_executable(text
    Server.cpp
    cJSON.c
    analysis.cpp
    U_Server.cpp
)
target_link_libraries(text PRIVATE Threads::Threads)