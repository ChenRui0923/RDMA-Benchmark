cmake_minimum_required (VERSION 2.6)

project (rdma_benchmark)

set(PROJECT_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src)
set(CMAKE_BINARY_DIR  ${CMAKE_CURRENT_BINARY_DIR}/bin)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Release) # or debug if you need to use gdb on it.

# Some how g++ has better performance than clang++. Of course I don't know all flags for clang++.
set(CMAKE_CXX_COMPILER g++) # or clang++
#set(CMAKE_CXX_COMPILER clang++) # or clang++
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g ") # get crazy here
#add_compile_options("-std=c++17")
#set(CMAKE_CXX_FLAGS "-O0  -ggdb")

find_library(IBVERBS_LIBRARY ibverbs HINTS /lib/x86_64-linux-gnu/)
find_library(RDMACM_LIBRARY rdmacm HINTS /lib/x86_64-linux-gnu/)
link_libraries( ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})


add_executable(rc_send ${PROJECT_SOURCE_DIR}/rc_send.cpp)
add_executable(rc_pingpong ${PROJECT_SOURCE_DIR}/rc_pingpong.cpp)

add_executable(rc_write ${PROJECT_SOURCE_DIR}/rc_write.c)
add_executable(rc_read ${PROJECT_SOURCE_DIR}/rc_read.c)

add_executable(uc_pingpong ${PROJECT_SOURCE_DIR}/uc_pingpong.c)
add_executable(uc_write ${PROJECT_SOURCE_DIR}/uc_write.c)

add_executable(RDMA_RC_minLat_a ${PROJECT_SOURCE_DIR}/RDMA_RC_minLat_a.c)
add_executable(RDMA_RC_minLat_b ${PROJECT_SOURCE_DIR}/RDMA_RC_minLat_b.c)