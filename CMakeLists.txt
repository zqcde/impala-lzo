# Copyright (c) 2011 Cloudera, Inc. All rights reserved.

cmake_minimum_required(VERSION 2.6)

set(CMAKE_MODULE_PATH cmake_modules)
SET(CXX_COMMON_FLAGS "-msse4.2 -Wall -Wno-sign-compare -pthread")
SET(CXX_COMMON_FLAGS "${CXX_COMMON_FLAGS} -DBOOST_DATE_TIME_POSIX_TIME_STD_CONFIG")
SET(CXX_FLAGS_DEBUG "-ggdb -Wall")
SET(CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG -Wno-strict-aliasing")

# if no build build type is specified, default to debug builds
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif(NOT CMAKE_BUILD_TYPE)

if ("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG")
  SET(CMAKE_CXX_FLAGS ${CXX_FLAGS_DEBUG})
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
  SET(CMAKE_CXX_FLAGS ${CXX_FLAGS_RELEASE})
endif()

SET(CMAKE_CXX_FLAGS "${CXX_COMMON_FLAGS} ${CMAKE_CXX_FLAGS}")

# Locate the LZO compression library.
find_package(Lzo REQUIRED)

# Locate the Thrift headers.
find_package(Thrift REQUIRED)

add_definitions(-DHAVE_INTTYPES_H -DHAVE_NETINET_IN_H)

# where to put generated libraries
set(BUILD_OUTPUT_ROOT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")
set(LIBRARY_OUTPUT_PATH "${BUILD_OUTPUT_ROOT_DIRECTORY}")

include_directories(${LZO_INCLUDE_DIR})
include_directories($ENV{IMPALA_HOME}/be/src)
include_directories($ENV{IMPALA_HOME}/be/generated-sources)
include_directories($ENV{IMPALA_HOME}/thirdparty)
include_directories($ENV{IMPALA_HOME}/thirdparty/hadoop-$ENV{IMPALA_HADOOP_VERSION}/include)
include_directories($ENV{IMPALA_HOME}/thirdparty/glog-$ENV{IMPALA_GLOG_VERSION}/src)
include_directories($ENV{IMPALA_HOME}/thirdparty/gflags-$ENV{IMPALA_GFLAGS_VERSION}/src)
include_directories(${THRIFT_INCLUDE_DIR})

message(STATUS "LZO lib: ${LZO_STATIC_LIB}")
add_library(impalalzo SHARED
  hdfs-lzo-text-scanner.cc
)

target_link_libraries(impalalzo
  ${LZO_LIB}
)

