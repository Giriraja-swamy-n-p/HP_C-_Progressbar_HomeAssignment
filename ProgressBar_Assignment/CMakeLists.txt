cmake_minimum_required(VERSION 3.16)
project(ProgressBarApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ProgressBar STATIC
    src/ProgressWindow.cpp
)
target_include_directories(ProgressBar PUBLIC include)

add_executable(ProgressBarApp
    src/main.cpp
)
target_link_libraries(ProgressBarApp PRIVATE ProgressBar)

if(MSVC)
    # Static CRT so the exe runs on a clean machine without the VC++ redistributable.
    # The library uses the same runtime so other projects can link it with /MT.
    set_property(TARGET ProgressBar ProgressBarApp PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(ProgressBar PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(ProgressBarApp PRIVATE /W4 /permissive- /utf-8)
    target_link_options(ProgressBarApp PRIVATE
        /SUBSYSTEM:CONSOLE
        /MANIFEST:EMBED
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
else()
    target_compile_options(ProgressBar PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ProgressBarApp PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ProgressBar PUBLIC comctl32 user32 gdi32)
endif()

if(WIN32 AND NOT MSVC)
    target_sources(ProgressBarApp PRIVATE app.manifest)
endif()
