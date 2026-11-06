# demodump <file.dem>: the whole parsed Demo as JSON on stdout
add_executable(demodump demodump.cpp)
target_link_libraries(demodump PRIVATE p2demo)
if(MSVC)
	target_compile_options(demodump PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(demodump PRIVATE -Wall -Wextra -Wpedantic)
endif()
