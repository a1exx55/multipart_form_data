cmake_minimum_required(VERSION 3.27.7)

project(multipart_form_data)

#set flags for compiler
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -lssl -lcrypto")
set(CMAKE_CXX_FLAGS_DEBUG "-Wall -Wextra -g -pedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

#add external libs
find_package(OpenSSL REQUIRED)
 
#include all source files
set(SRC 
	src/main.cpp
	)

add_executable(${PROJECT_NAME} ${SRC})

target_include_directories(${PROJECT_NAME} PRIVATE "src/")

#link external libs
target_link_libraries(${PROJECT_NAME} 
	OpenSSL::SSL OpenSSL::Crypto) 