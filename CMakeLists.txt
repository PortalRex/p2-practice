# P2 Practice.
#   cmake -S . -B build\x86 -G "Visual Studio 16 2019" -A Win32
#   cmake --build build\x86 --config Release
cmake_minimum_required(VERSION 3.15)
project(p2practice LANGUAGES CXX)

# Everything links the static CRT (/MT), as the plugin DLL must.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# -DP2_ASAN=ON: AddressSanitizer for every target (for tools/demostress)
option(P2_ASAN "Build with AddressSanitizer" OFF)
if(P2_ASAN)
	if(MSVC)
		add_compile_options(/fsanitize=address /Zi)
		add_link_options(/INCREMENTAL:NO /DEBUG)
	else()
		add_compile_options(-fsanitize=address,undefined -g)
		add_link_options(-fsanitize=address,undefined)
	endif()
endif()

add_subdirectory(core)
add_subdirectory(tools/demodump)
add_subdirectory(tools/demostress)

# The route builder and the Portal 2 plugin (the plugin itself only in 32-bit builds), and the launcher's helper
add_subdirectory(plugin)
add_subdirectory(tools/routeinfo)
add_subdirectory(tools/ghosttest)
