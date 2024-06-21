cmake_minimum_required(VERSION 3.27.7)

project(multipart_form_data)

#set flags for compiler
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
set(CMAKE_CXX_FLAGS_DEBUG "-Wall -Wextra -g -pedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
 
#include all source files
set(SRC 
	examples/main.cpp
	)

add_executable(${PROJECT_NAME} ${SRC})

target_include_directories(${PROJECT_NAME} PRIVATE "src/" "examples/")