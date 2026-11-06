# The route builder without the engine: shared by the plugin and the command-line tools
add_library(p2route_lib STATIC src/route.cpp src/maps.cpp)
target_compile_features(p2route_lib PUBLIC cxx_std_17)
target_include_directories(p2route_lib PUBLIC src)
target_link_libraries(p2route_lib PUBLIC p2demo)
target_compile_definitions(p2route_lib PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(MSVC)
	target_compile_options(p2route_lib PRIVATE /W4 /permissive- /utf-8)
	set_property(TARGET p2route_lib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The Portal 2 server plugin (p2practice.dll). Portal 2 is a 32-bit game: configure with -A Win32.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(STATUS "p2practice plugin skipped: it must be built 32-bit (-A Win32)")
	return()
endif()

add_library(p2practice SHARED
	src/engine.cpp
	src/ghostmodel.cpp
	src/plugin.cpp
	src/trainer.cpp
)
target_compile_features(p2practice PRIVATE cxx_std_17)
target_link_libraries(p2practice PRIVATE p2route_lib)
target_compile_definitions(p2practice PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
	target_compile_options(p2practice PRIVATE /W4 /permissive- /utf-8 /Zi)
	# a .map and .pdb beside the DLL: crash reports give an address inside the DLL, these turn it into a function
	target_link_options(p2practice PRIVATE /MAP /DEBUG /OPT:REF /OPT:ICF)
	# static CRT: the DLL must load on a PC that has no Visual C++ runtime installed
	set_property(TARGET p2practice PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
