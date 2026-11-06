# demostress: feeds cut-short and corrupted copies of demos through
# p2demo::Parse (run it from a -DP2_ASAN=ON build), and times the parser
add_executable(demostress demostress.cpp)
target_link_libraries(demostress PRIVATE p2demo)
if(MSVC)
	target_compile_options(demostress PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(demostress PRIVATE -Wall -Wextra -Wpedantic)
endif()
