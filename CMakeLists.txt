cmake_minimum_required(VERSION 3.20)

if(APPLE)
    project(FloopHole VERSION 1.0.0 LANGUAGES C CXX OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment version")
else()
    project(FloopHole VERSION 1.0.0 LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LUNASVG_DISABLE_LOAD_SYSTEM_FONTS ON CACHE BOOL "" FORCE)
FetchContent_Declare(lunasvg
    GIT_REPOSITORY https://github.com/sammycage/lunasvg.git
    GIT_TAG 7b6970f40e72826749d4b050d11c2a4c3406f10c
    GIT_SUBMODULES plutovg
)
FetchContent_MakeAvailable(lunasvg)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/floophole_logo.svg" FLOOPHOLE_LOGO)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/floophole_logo.svg")
configure_file(src/FloopHoleAssets.h.in generated/FloopHoleAssets.h @ONLY)

set(VST3_PLUGINTERFACES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

foreach(SDK_FILE IN ITEMS
    base/funknown.h
    base/funknown.cpp
    base/ustring.cpp
    base/conststringtable.cpp
    gui/iplugview.h
    vst/ivstaudioprocessor.h
    vst/ivsteditcontroller.h
)
    if(NOT EXISTS "${VST3_PLUGINTERFACES_DIR}/pluginterfaces/${SDK_FILE}")
        message(FATAL_ERROR
            "Steinberg VST3 interfaces are missing or incomplete. "
            "Run: cmake -P \"${CMAKE_CURRENT_SOURCE_DIR}/scripts/init.cmake\"")
    endif()
endforeach()

set(PLUGINTERFACES_SOURCES
    src/vst3_iids.cpp
    third_party/pluginterfaces/base/funknown.cpp
    third_party/pluginterfaces/base/ustring.cpp
    third_party/pluginterfaces/base/conststringtable.cpp
)

set(FLBRIDGE_SHARED_SOURCES
    src/FLPluginInstance.cpp
    src/FLHostServices.cpp
    src/FLPluginScanner.cpp
    src/NativePluginCatalog.cpp
    src/FLPathManager.cpp
)

if(APPLE)
    set(FLBRIDGE_SOURCES
        ${FLBRIDGE_SHARED_SOURCES}
        src/FLPluginScanner.mm
        src/FLPluginInstance.mm
        src/VST3Plugin.mm
        src/ToolbarControls_mac.mm
        ${PLUGINTERFACES_SOURCES}
    )

    add_library(FloopHole MODULE ${FLBRIDGE_SOURCES})

    target_include_directories(FloopHole PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${VST3_PLUGINTERFACES_DIR}
    )

    target_compile_definitions(FloopHole PRIVATE
        RELEASE=1
        SMTG_USE_STDATOMIC_H=1
    )

    target_compile_options(FloopHole PRIVATE
        -fobjc-arc
        -Wno-deprecated-declarations
    )

    target_link_libraries(FloopHole PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework CoreFoundation"
        "-framework Accelerate"
        "-framework AudioToolbox"
    )

    set_target_properties(FloopHole PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "vst3"
        OUTPUT_NAME "FloopHole"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/Info.plist.in"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3"
    )

    # Create PkgInfo inside the .vst3 bundle, optionally cross-compile the Windows x86_64 VST3 binary, and ad-hoc codesign
    find_program(MINGW_GXX x86_64-w64-mingw32-g++)
    if(MINGW_GXX)
        add_custom_target(FloopHole_Windows ALL
            COMMAND ${CMAKE_COMMAND} -B "${CMAKE_BINARY_DIR}/windows-x64"
                -S "${CMAKE_CURRENT_SOURCE_DIR}"
                -DCMAKE_TOOLCHAIN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/cmake/mingw-w64-x86_64.cmake"
                -DCMAKE_BUILD_TYPE=Release
            COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}/windows-x64" --config Release -j4
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:FloopHole>/x86_64-win"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/windows-x64/VST3/FloopHole.vst3/Contents/x86_64-win/FloopHole.vst3"
                "$<TARGET_BUNDLE_CONTENT_DIR:FloopHole>/x86_64-win/FloopHole.vst3"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/windows-x64/VST3/FloopHole.vst3/Contents/x86_64-win/FloopHole.vst3"
                "${CMAKE_BINARY_DIR}/VST3/FloopHole_x64.vst3"
            COMMAND /usr/bin/codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:FloopHole>"
            DEPENDS FloopHole
                src/FLPluginScanner_win32.cpp
                src/FLPluginInstance_win32.cpp
                src/VST3Plugin_win32.cpp
                src/ToolbarControls_win32.cpp
                src/FloopHole_win32.def
            COMMENT "Building Windows x86_64 VST3 binary and embedding into universal VST3 bundle"
        )
    endif()

    add_custom_command(TARGET FloopHole POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo_append "BNDL????" > "$<TARGET_BUNDLE_CONTENT_DIR:FloopHole>/PkgInfo"
        COMMENT "Finalizing FloopHole.vst3 bundle"
    )

    # Standalone VST3 Verification Test Suite (macOS)
    add_executable(test_vst3
        src/test_vst3.mm
        ${FLBRIDGE_SHARED_SOURCES}
        src/FLPluginScanner.mm
        src/FLPluginInstance.mm
        ${PLUGINTERFACES_SOURCES}
    )

    target_include_directories(test_vst3 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${VST3_PLUGINTERFACES_DIR}
    )

    target_compile_definitions(test_vst3 PRIVATE
        RELEASE=1
        SMTG_USE_STDATOMIC_H=1
    )

    target_compile_options(test_vst3 PRIVATE
        -fobjc-arc
        -Wno-deprecated-declarations
    )

    target_link_libraries(test_vst3 PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework CoreFoundation"
        "-framework Accelerate"
        "-framework AudioToolbox"
    )
elseif(WIN32)
    add_compile_definitions(NOMINMAX)

    add_executable(FloopHole_probe
        src/test_win32.cpp
        ${FLBRIDGE_SHARED_SOURCES}
        src/FLPluginScanner_win32.cpp
        src/FLPluginInstance_win32.cpp
    )
    target_compile_definitions(FloopHole_probe PRIVATE _CRT_SECURE_NO_WARNINGS=1)
    target_link_libraries(FloopHole_probe PRIVATE user32 bcrypt advapi32 ole32 shell32 comdlg32 mfplat mfreadwrite mfuuid)
    if(MINGW)
        target_link_options(FloopHole_probe PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    set(FLBRIDGE_WIN32_SOURCES
        ${FLBRIDGE_SHARED_SOURCES}
        src/FLPluginScanner_win32.cpp
        src/FLPluginInstance_win32.cpp
        src/VST3Plugin_win32.cpp
        src/ToolbarControls_win32.cpp
        src/FloopHole_win32.def
        ${PLUGINTERFACES_SOURCES}
    )

    add_library(FloopHole MODULE ${FLBRIDGE_WIN32_SOURCES})

    target_include_directories(FloopHole PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${VST3_PLUGINTERFACES_DIR}
    )

    target_compile_definitions(FloopHole PRIVATE
        RELEASE=1
        _CRT_SECURE_NO_WARNINGS=1
    )

    if(MINGW)
        target_link_options(FloopHole PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
            -Wl,--kill-at
            -Wl,-s
        )
    endif()

    target_link_libraries(FloopHole PRIVATE
        user32
        gdi32
        bcrypt
        gdiplus
        comdlg32
        advapi32
        ole32
        uuid
        shell32
        mfplat
        mfreadwrite
        mfuuid
    )

    # Output both a flat FloopHole.vst3 binary and a standard Windows VST3 3.7+ bundle:
    #   VST3/FloopHole.vst3/Contents/x86_64-win/FloopHole.vst3
    set_target_properties(FloopHole PROPERTIES
        PREFIX ""
        OUTPUT_NAME "FloopHole"
        SUFFIX ".vst3"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/VST3/FloopHole.vst3/Contents/x86_64-win>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/VST3/FloopHole.vst3/Contents/x86_64-win>"
    )

    add_custom_command(TARGET FloopHole POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:FloopHole>"
            "${CMAKE_BINARY_DIR}/VST3/FloopHole_x64.vst3"
        COMMENT "Packaging Windows x86_64 VST3 bundle (VST3/FloopHole.vst3/Contents/x86_64-win/FloopHole.vst3)"
    )
endif()

target_sources(FloopHole PRIVATE src/LogoRenderer.cpp)
target_include_directories(FloopHole PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(FloopHole PRIVATE lunasvg::lunasvg)
set(FLOOPHOLE_RESOURCES "${CMAKE_BINARY_DIR}/VST3/FloopHole.vst3/Contents/Resources")
set_property(TARGET FloopHole APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${lunasvg_SOURCE_DIR}/LICENSE"
    "${lunasvg_SOURCE_DIR}/plutovg/LICENSE"
    "${VST3_PLUGINTERFACES_DIR}/pluginterfaces/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
)
add_custom_command(TARGET FloopHole POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FLOOPHOLE_RESOURCES}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${FLOOPHOLE_RESOURCES}/licenses/FloopHole-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${lunasvg_SOURCE_DIR}/LICENSE" "${FLOOPHOLE_RESOURCES}/licenses/LunaSVG-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${lunasvg_SOURCE_DIR}/plutovg/LICENSE" "${FLOOPHOLE_RESOURCES}/licenses/PlutoVG-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VST3_PLUGINTERFACES_DIR}/pluginterfaces/LICENSE.txt" "${FLOOPHOLE_RESOURCES}/licenses/VST3-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${lunasvg_SOURCE_DIR}/plutovg/source/FTL.TXT" "${FLOOPHOLE_RESOURCES}/licenses/FTL.TXT"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt" "${FLOOPHOLE_RESOURCES}/licenses/THIRD_PARTY_NOTICES.txt"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${FLOOPHOLE_RESOURCES}/moduleinfo.json"
    COMMENT "Packaging licenses; dynamic classes deliberately have no moduleinfo.json"
)
if(APPLE)
    add_custom_command(TARGET FloopHole POST_BUILD
        COMMAND /usr/bin/codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:FloopHole>"
        COMMENT "Signing complete FloopHole bundle"
    )
else()
    add_custom_command(TARGET FloopHole POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${FLOOPHOLE_RESOURCES}/licenses" "${CMAKE_BINARY_DIR}/VST3/FloopHole-licenses"
        COMMAND ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/VST3/FloopHole_x64.zip" --format=zip FloopHole_x64.vst3 FloopHole-licenses
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/VST3"
        COMMENT "Creating licensed standalone Windows distribution"
    )
endif()

include(CTest)
if(BUILD_TESTING AND (APPLE OR WIN32))
    if(APPLE)
        set(FLBRIDGE_INSTANCE_PLATFORM_SOURCES src/FLPluginInstance.mm src/FLPluginScanner.mm)
    else()
        set(FLBRIDGE_INSTANCE_PLATFORM_SOURCES src/FLPluginInstance_win32.cpp src/FLPluginScanner_win32.cpp)
    endif()
    add_executable(test_bridge
        src/test_bridge.cpp
        ${FLBRIDGE_SHARED_SOURCES}
        ${FLBRIDGE_INSTANCE_PLATFORM_SOURCES}
    )
    target_include_directories(test_bridge PRIVATE src)
    if(APPLE)
        target_compile_options(test_bridge PRIVATE -fobjc-arc -Wno-deprecated-declarations)
        target_link_libraries(test_bridge PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Accelerate" "-framework AudioToolbox")
    else()
        target_compile_definitions(test_bridge PRIVATE _CRT_SECURE_NO_WARNINGS=1)
        target_link_libraries(test_bridge PRIVATE user32 bcrypt advapi32 ole32 shell32 comdlg32 mfplat mfreadwrite mfuuid)
        if(MINGW)
            target_link_options(test_bridge PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endif()
    add_test(NAME bridge_contracts COMMAND test_bridge)
    if(WIN32)
        add_executable(test_windows_editor
            src/test_windows_editor.cpp
            src/ToolbarControls_win32.cpp
            src/LogoRenderer.cpp
        )
        target_include_directories(test_windows_editor PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(test_windows_editor PRIVATE user32 gdi32 gdiplus lunasvg::lunasvg)
        if(MINGW)
            target_link_options(test_windows_editor PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME windows_editor_lifecycle COMMAND test_windows_editor)
    endif()
endif()
