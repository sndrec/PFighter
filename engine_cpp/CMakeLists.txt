cmake_minimum_required(VERSION 3.24)

project(PFighterEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PFIGHTER_FETCH_RAYLIB "Fetch raylib with CMake FetchContent when it is not installed" OFF)
option(PFIGHTER_BUILD_RAYLIB_APP "Build the raylib prototype app when raylib is available" ON)
set(PFIGHTER_RAYLIB_DIR "" CACHE PATH "Path to an extracted raylib package containing include/ and lib/")

add_library(pfighter_core
    src/core/action.cpp
    src/core/animation.cpp
    src/core/animation_asset.cpp
    src/core/fighter_data.cpp
    src/core/fighter_package.cpp
    src/core/hsd_action_import.cpp
    src/core/math.cpp
    src/core/replay.cpp
    src/core/simulation.cpp
    src/core/state_functions.cpp
    src/editor/fighter_editor.cpp
)

target_include_directories(pfighter_core PUBLIC src)

add_executable(pfighter_headless src/app/headless_main.cpp)
target_link_libraries(pfighter_headless PRIVATE pfighter_core)

add_executable(pfighter_package_converter src/app/package_converter_main.cpp)
target_link_libraries(pfighter_package_converter PRIVATE pfighter_core)

if(MINGW)
    get_filename_component(_pfighter_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_pfighter_mingw_runtime_dlls
        "${_pfighter_mingw_bin}/libstdc++-6.dll"
        "${_pfighter_mingw_bin}/libgcc_s_seh-1.dll"
        "${_pfighter_mingw_bin}/libwinpthread-1.dll"
    )

    function(pfighter_copy_mingw_runtime target_name)
        foreach(_runtime_dll IN LISTS _pfighter_mingw_runtime_dlls)
            if(EXISTS "${_runtime_dll}")
                get_filename_component(_runtime_name "${_runtime_dll}" NAME)
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_runtime_dll}"
                        "$<TARGET_FILE_DIR:${target_name}>/${_runtime_name}"
                )
            endif()
        endforeach()
    endfunction()

    pfighter_copy_mingw_runtime(pfighter_headless)
    pfighter_copy_mingw_runtime(pfighter_package_converter)
endif()

if(PFIGHTER_BUILD_RAYLIB_APP)
    find_package(raylib QUIET)

    if(NOT raylib_FOUND AND NOT TARGET raylib)
        set(_pfighter_raylib_candidates
            "${PFIGHTER_RAYLIB_DIR}"
        )

        if(WIN32)
            list(APPEND _pfighter_raylib_candidates
                "${CMAKE_CURRENT_LIST_DIR}/../raylib-6.0_win64_mingw-w64"
                "${CMAKE_CURRENT_LIST_DIR}/../raylib-6.0_win64_msvc16"
            )
        elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            list(APPEND _pfighter_raylib_candidates
                "${CMAKE_CURRENT_LIST_DIR}/../raylib-6.0_linux_amd64"
            )
        endif()

        foreach(_candidate IN LISTS _pfighter_raylib_candidates)
            if(_candidate
                AND EXISTS "${_candidate}/include/raylib.h"
                AND (EXISTS "${_candidate}/lib/libraylib.so.6.0.0"
                    OR EXISTS "${_candidate}/lib/libraylib.so"
                    OR EXISTS "${_candidate}/lib/libraylib.a"
                    OR EXISTS "${_candidate}/lib/libraylibdll.a"
                    OR EXISTS "${_candidate}/lib/raylib.lib"
                    OR EXISTS "${_candidate}/lib/raylibdll.lib"))
                if(MINGW AND EXISTS "${_candidate}/lib/libraylibdll.a")
                    set(_pfighter_raylib_lib "${_candidate}/lib/libraylibdll.a")
                elseif(UNIX AND EXISTS "${_candidate}/lib/libraylib.a")
                    set(_pfighter_raylib_lib "${_candidate}/lib/libraylib.a")
                elseif(UNIX AND EXISTS "${_candidate}/lib/libraylib.so.6.0.0")
                    set(_pfighter_raylib_lib "${_candidate}/lib/libraylib.so.6.0.0")
                elseif(UNIX AND EXISTS "${_candidate}/lib/libraylib.so")
                    set(_pfighter_raylib_lib "${_candidate}/lib/libraylib.so")
                elseif(EXISTS "${_candidate}/lib/libraylib.a")
                    set(_pfighter_raylib_lib "${_candidate}/lib/libraylib.a")
                elseif(EXISTS "${_candidate}/lib/raylib.lib")
                    set(_pfighter_raylib_lib "${_candidate}/lib/raylib.lib")
                else()
                    set(_pfighter_raylib_lib "${_candidate}/lib/raylibdll.lib")
                endif()

                add_library(raylib UNKNOWN IMPORTED)
                set_target_properties(raylib PROPERTIES
                    IMPORTED_LOCATION "${_pfighter_raylib_lib}"
                    INTERFACE_INCLUDE_DIRECTORIES "${_candidate}/include"
                )
                if(EXISTS "${_candidate}/lib/raylib.dll")
                    set(PFIGHTER_RAYLIB_DLL "${_candidate}/lib/raylib.dll")
                endif()
                if(WIN32)
                    target_link_libraries(raylib INTERFACE opengl32 gdi32 winmm)
                elseif(UNIX)
                    target_link_libraries(raylib INTERFACE m dl pthread GL X11 Xrandr Xi Xinerama Xcursor)
                endif()
                message(STATUS "Using local raylib package: ${_candidate}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT raylib_FOUND AND NOT TARGET raylib AND PFIGHTER_FETCH_RAYLIB)
        include(FetchContent)
        FetchContent_Declare(
            raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG 6.0
        )
        FetchContent_MakeAvailable(raylib)
    endif()

    if(TARGET raylib OR raylib_FOUND)
        add_executable(pfighter_raylib
            src/app/raylib_main.cpp
        )
        target_link_libraries(pfighter_raylib PRIVATE pfighter_core raylib)
        target_compile_definitions(pfighter_raylib PRIVATE PFIGHTER_WITH_RAYLIB=1)
        if(PFIGHTER_RAYLIB_DLL)
            add_custom_command(TARGET pfighter_raylib POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PFIGHTER_RAYLIB_DLL}"
                    "$<TARGET_FILE_DIR:pfighter_raylib>/raylib.dll"
            )
        endif()
        if(MINGW)
            pfighter_copy_mingw_runtime(pfighter_raylib)
        endif()
    else()
        message(STATUS "raylib was not found. Build with -DPFIGHTER_FETCH_RAYLIB=ON or install raylib to enable pfighter_raylib.")
    endif()
endif()
