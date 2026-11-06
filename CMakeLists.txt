cmake_minimum_required(VERSION 3.20)
project(fix-proto CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(quickfix CONFIG REQUIRED)

add_executable(acceptor acceptor.cpp)
target_link_libraries(acceptor PRIVATE quickfix)

add_executable(initiator initiator.cpp)
target_link_libraries(initiator PRIVATE quickfix)