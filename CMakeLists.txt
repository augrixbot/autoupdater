cmake_minimum_required(VERSION 3.16)
project(AugrixUpdater VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # MSVC: static runtime for portable standalone .exe
    add_compile_options(/W3 /utf-8)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
    # MinGW (cross-compile from Linux or native): static linkage so no DLL deps
    add_compile_options(-Wall -O2)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

add_executable(AugrixUpdater src/main.cpp)

target_include_directories(AugrixUpdater PRIVATE include)

target_link_libraries(AugrixUpdater PRIVATE
    winhttp ws2_32 shlwapi advapi32
    ole32 oleaut32 credui
)

set_target_properties(AugrixUpdater PROPERTIES OUTPUT_NAME "AugrixUpdater")
