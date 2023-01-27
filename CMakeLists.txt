cmake_minimum_required(VERSION 3.0.0)
project(lock_or_rte VERSION 0.1.0)

SET(CMAKE_BUILD_TYPE "Release")
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3")
    message(STATUS "CMAKE_BUILD_TYPE = Release")
endif()
set(CMAKE_CXX_STANDARD 17)

find_package(unordered_dense CONFIG REQUIRED)

add_executable(locktest lock.cc)
target_link_libraries(locktest pthread unordered_dense::unordered_dense)

add_executable(ringmpsctest ring_mpsc.cc)
target_link_libraries(ringmpsctest pthread unordered_dense::unordered_dense)

add_executable(ringspsctest ring_spsc.cc)
target_link_libraries(ringspsctest pthread unordered_dense::unordered_dense)

add_executable(ringccqtest ring_concurrentqueue.cc)
target_link_libraries(ringccqtest pthread unordered_dense::unordered_dense)

add_executable(ringrwqtest ring_readerwriterqueue.cc)
target_link_libraries(ringrwqtest pthread unordered_dense::unordered_dense)

add_executable(ringrwqtimetest ring_rwq_time.cc)
target_link_libraries(ringrwqtimetest pthread unordered_dense::unordered_dense)

add_executable(ringccqtimetest ring_ccq_time.cc)
target_link_libraries(ringccqtimetest pthread unordered_dense::unordered_dense)