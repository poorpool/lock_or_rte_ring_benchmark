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

add_executable(ring_mpsc_rte_test ring_mpsc_rte.cc)
target_link_libraries(ring_mpsc_rte_test pthread unordered_dense::unordered_dense)

add_executable(ring_spsc_rte_test ring_spsc_rte.cc)
target_link_libraries(ring_spsc_rte_test pthread unordered_dense::unordered_dense)

add_executable(ring_mpsc_moody_test ring_mpsc_moody.cc)
target_link_libraries(ring_mpsc_moody_test pthread unordered_dense::unordered_dense)

add_executable(ring_spsc_moody_test ring_spsc_moody.cc)
target_link_libraries(ring_spsc_moody_test pthread unordered_dense::unordered_dense)

# add_executable(ringrwqtimetest ring_rwq_time.cc)
# target_link_libraries(ringrwqtimetest pthread unordered_dense::unordered_dense)

# add_executable(ringccqtimetest ring_ccq_time.cc)
# target_link_libraries(ringccqtimetest pthread unordered_dense::unordered_dense)

add_executable(ring_time_mpsc_rte ring_rdtsc_mpsc_rte.cc)
target_link_libraries(ring_time_mpsc_rte pthread unordered_dense::unordered_dense)
add_executable(ring_time_spsc_rte ring_rdtsc_spsc_rte.cc)
target_link_libraries(ring_time_spsc_rte pthread unordered_dense::unordered_dense)

add_executable(lock_rocksdb
    lock_rocksdb.cc
)
target_link_libraries(lock_rocksdb 
    rocksdb lz4 pthread -lz -lsnappy -lbz2 -lzstd -ldl)

add_executable(ring_spsc_rocksdb ring_spsc_rocksdb.cc)
target_link_libraries(ring_spsc_rocksdb
    pthread unordered_dense::unordered_dense
    rocksdb lz4 pthread -lz -lsnappy -lbz2 -lzstd -ldl)
