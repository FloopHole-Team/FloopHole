cmake_minimum_required(VERSION 3.20)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SDK_DIR "${PROJECT_ROOT}/third_party/pluginterfaces")
set(SDK_REPOSITORY "https://github.com/steinbergmedia/vst3_pluginterfaces.git")
set(SDK_REVISION "4f547e8e102b47de4a8b8aaf343c73b700786372")
set(SDK_REQUIRED_FILES
    base/funknown.h
    base/funknown.cpp
    base/ustring.cpp
    base/conststringtable.cpp
    gui/iplugview.h
    vst/ivstaudioprocessor.h
    vst/ivsteditcontroller.h
)

set(SDK_COMPLETE TRUE)
foreach(SDK_FILE IN LISTS SDK_REQUIRED_FILES)
    if(NOT EXISTS "${SDK_DIR}/${SDK_FILE}")
        set(SDK_COMPLETE FALSE)
    endif()
endforeach()

if(SDK_COMPLETE)
    message(STATUS "Using existing VST3 interfaces: ${SDK_DIR}")
    return()
endif()

if(EXISTS "${SDK_DIR}")
    message(FATAL_ERROR
        "An incomplete SDK already exists at ${SDK_DIR}. "
        "Move it aside or remove it, then rerun this script. No files were changed.")
endif()

find_package(Git REQUIRED)
set(SDK_DOWNLOAD_DIR "${PROJECT_ROOT}/third_party/.pluginterfaces-download")
if(EXISTS "${SDK_DOWNLOAD_DIR}")
    message(FATAL_ERROR
        "A previous download directory exists at ${SDK_DOWNLOAD_DIR}. "
        "Ensure no other initializer is running, then move it aside or remove it and retry.")
endif()

function(run_git)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${ARGV}
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT STREQUAL "0")
        file(REMOVE_RECURSE "${SDK_DOWNLOAD_DIR}")
        message(FATAL_ERROR "SDK download failed (${GIT_RESULT}). Check your network and rerun this script.")
    endif()
endfunction()

message(STATUS "Downloading Steinberg VST3 SDK 3.8.1 interfaces (${SDK_REVISION})")
run_git(init "${SDK_DOWNLOAD_DIR}")
run_git(-C "${SDK_DOWNLOAD_DIR}" fetch --depth 1 "${SDK_REPOSITORY}" "${SDK_REVISION}")
run_git(-C "${SDK_DOWNLOAD_DIR}" checkout --detach FETCH_HEAD)

foreach(SDK_FILE IN LISTS SDK_REQUIRED_FILES)
    if(NOT EXISTS "${SDK_DOWNLOAD_DIR}/${SDK_FILE}")
        file(REMOVE_RECURSE "${SDK_DOWNLOAD_DIR}")
        message(FATAL_ERROR "Downloaded SDK is missing ${SDK_FILE}.")
    endif()
endforeach()

file(RENAME "${SDK_DOWNLOAD_DIR}" "${SDK_DIR}")
message(STATUS "SDK ready: ${SDK_DIR}")