# p2demo: the Portal 2 demo reader, a dependency-free static library.
#   add_subdirectory(core)  ->  target_link_libraries(<target> PRIVATE p2demo)
cmake_minimum_required(VERSION 3.15)
project(p2demo LANGUAGES CXX)

add_library(p2demo STATIC
	include/p2demo/demo.h
	src/datatables.cpp
	src/datatables.h
	src/demo.cpp
	src/entities.cpp
	src/entities.h
	src/netmessages.cpp
	src/netmessages.h
	src/props.cpp
	src/props.h
	src/readers.h
	src/sardata.cpp
	src/sardata.h
	src/stringtables.cpp
	src/stringtables.h
	src/usercmd.cpp
	src/usercmd.h
)

add_library(p2demo::p2demo ALIAS p2demo)

target_include_directories(p2demo PUBLIC include)
target_compile_features(p2demo PUBLIC cxx_std_17)
set_target_properties(p2demo PROPERTIES
	CXX_EXTENSIONS OFF
	# static CRT: the library goes into a plugin DLL that must not depend on the VC redistributable
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
	target_compile_options(p2demo PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(p2demo PRIVATE -Wall -Wextra -Wpedantic)
endif()
