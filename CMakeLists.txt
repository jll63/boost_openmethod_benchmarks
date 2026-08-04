# Boost.OpenMethod dispatch benchmark.
#
# OpenMethod is taken from ./include, a symlink into the working tree, so that
# edits there are picked up without reinstalling. Every other Boost header comes
# from the system-wide installation in /usr/local/include, which is already on
# the compiler's default search path -- the library is header-only, so no
# find_package and nothing to link.
cmake_minimum_required(VERSION 3.16)
project(openmethod-benchmark CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(OMB_CLASSES 100 CACHE STRING "Number of leaf classes in the hierarchy")
set(OMB_BITS 64 CACHE STRING "Target bitness: 64 or 32 (-m32 needs g++-multilib)")

add_executable(benchmark src/main.cpp)

target_include_directories(benchmark PRIVATE include src)
target_compile_definitions(benchmark PRIVATE OMB_CLASSES=${OMB_CLASSES})

# -march=native for clflushopt; no stack protector so the timed function holds
# nothing but the dispatch.
target_compile_options(benchmark PRIVATE
  -O2 -march=native -m${OMB_BITS} -fno-stack-protector -fcf-protection=none
  -Wall -Wextra)
target_link_options(benchmark PRIVATE -m${OMB_BITS})

enable_testing()
add_test(NAME verify COMMAND benchmark --verify)
