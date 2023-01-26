cmake_minimum_required(VERSION 3.0.0)
project(lock_or_rte VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)

find_package(unordered_dense CONFIG REQUIRED)

add_executable(locktest lock.cc)
target_link_libraries(locktest pthread unordered_dense::unordered_dense)
