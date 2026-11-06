# p2route.exe: describes demos as JSON for the launcher
add_executable(p2route main.cpp)
target_link_libraries(p2route PRIVATE p2route_lib)
target_compile_definitions(p2route PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
	target_compile_options(p2route PRIVATE /W4 /permissive- /utf-8)
	set_property(TARGET p2route PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
