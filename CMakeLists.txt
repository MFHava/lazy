
#          Copyright Michael Florian Hava.
# Distributed under the Boost Software License, Version 1.0.
#    (See accompanying file LICENSE_1_0.txt or copy at
#          http://www.boost.org/LICENSE_1_0.txt)

cmake_minimum_required(VERSION 3.26)
project(Lazy)

add_library(lazy INTERFACE)
	target_compile_features(lazy INTERFACE cxx_std_20)
	target_include_directories(lazy INTERFACE "inc")
	if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
		target_compile_options(lazy INTERFACE -Wall -Wextra -Wpedantic -Wconversion)
		target_compile_options(lazy INTERFACE -fsanitize=address)
		target_link_options(lazy INTERFACE -fsanitize=address)
	elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
		target_compile_options(lazy INTERFACE
			# warning flags
			/W4 # enable all sensible warnings
			/wd4003 # ignore not enough parameters for function macro
			/wd4099 # ignore mixing struct and class keyword for same type
			/wd4456 # ignore local shadowing
			/wd4457 # ignore function parameter shadowing
			/wd4458 # ignore class member shadowing
			/wd4459 # ignore global shadowing
			/we4700 # treat use of uninitialized variables as compile error
			# conformance flags
			/Zc:__cplusplus # update __cplusplus according to standard (stays at C++98 otherwise)
			/Zc:preprocessor # enable standard conformant preprocessor
			# miscellaneous flags
			/MP # parallel compilation
			/JMC # improve debugging support by only stepping into own code
			/bigobj # use "big object files" to circumvent class of errors that are related to heavy template or machine generated code
		)
	endif()

find_package(Catch2 3 REQUIRED)

add_executable(test-lazy)
	file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "inc/*")
		source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/inc FILES ${SRC})
		target_sources(test-lazy PRIVATE ${SRC})
	file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "test/*")
		source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/test FILES ${SRC})
		target_sources(test-lazy PRIVATE ${SRC})
	target_link_libraries(test-lazy PRIVATE lazy Catch2::Catch2WithMain)

