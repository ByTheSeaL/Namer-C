cmake_minimum_required(VERSION 3.15)
project(NamerC VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GUI subsystem (no console window), statically link the runtime so the exe
# is self-contained — a single small binary with no DLL dependencies.
add_executable(Namer WIN32
    src/main.cpp
    res/namer.rc
)

# Use the wide-character Win32 API everywhere (neither MSVC nor MinGW define
# these via CMake by default, so the *_GetItemText etc. macros would otherwise
# resolve to their ANSI variants).
target_compile_definitions(Namer PRIVATE UNICODE _UNICODE)

target_link_libraries(Namer PRIVATE comctl32 winhttp shlwapi)

if(MINGW)
    target_link_options(Namer PRIVATE -static -static-libgcc -static-libstdc++
                        -municode)
    # MinGW resource compiler needs the include dir for resource.h/windows.h
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/res")
elseif(MSVC)
    set_property(TARGET Namer PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(Namer PRIVATE /utf-8 /W3)
endif()

install(TARGETS Namer RUNTIME DESTINATION bin)
