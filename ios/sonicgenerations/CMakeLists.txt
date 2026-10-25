# Sonic Generations - ReXGlue Recompiled Project, iOS build.
#
# The game configuration comes from Player124413's
# Sonic-Generations-recomp-android-and-pc-edition; the runtime is the iOS port
# of the ReXGlue SDK, found through rexglue_DIR (see CMakePresets.json). SDK
# boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(sonicgenerations VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep build-machine paths (they contain the user name) out of the binary:
# source file names end up in log and assert messages.
add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=sonicgenerations")

include(generated/rexglue.cmake)

# The HedgeModManager mod loader (src/mods, tests/mod_loader_tests.cpp).
add_executable(sonicgenerations
    src/main.cpp
    src/frame_rate.cpp
    src/spin_waits.cpp
    src/mods/cri_utf.cpp
    src/mods/crilayla.cpp
    src/mods/cpk_overlay.cpp
    src/mods/hedge_archive.cpp
    src/mods/ini.cpp
    src/mods/mod_database.cpp
    src/mods/mod_loader.cpp)

# sonicgenerations_register.cpp is one flat function of function-pointer
# registrations, run once at startup, in a single basic block. LLVM is
# quadratic on it at -O1 and above, and so is -O0 on arm64, where GlobalISel's
# Localizer pass walks the whole block for every constant. -O0 with the classic
# instruction selector compiles it quickly. The -O3 precompiled header cannot be
# used at -O0, so it is skipped here.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sonicgenerations_register.cpp
    PROPERTIES COMPILE_OPTIONS "-O0;-mllvm;-global-isel=0" SKIP_PRECOMPILE_HEADERS ON)

if(APPLE)
    # CFLocale, for the game language default (src/sonicgenerations_app.h).
    target_link_libraries(sonicgenerations PRIVATE "-framework CoreFoundation")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    target_sources(sonicgenerations PRIVATE src/iso_import.mm src/mod_manager_ios.mm)
    set_source_files_properties(src/iso_import.mm src/mod_manager_ios.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(sonicgenerations PRIVATE "-framework UIKit" "-framework UniformTypeIdentifiers")
    # The GPU plugin is embedded in Frameworks/ below and loaded from there; a
    # loose copy next to the executable would not be covered by the signature.
    rexglue_setup_target(sonicgenerations)
else()
    rexglue_setup_target(sonicgenerations GPU_PLUGINS xenos)
endif()

# =============================================================================
#  iOS: a .app bundle that embeds the SDK's runtime dylibs. Game data is not
#  bundled; it goes in the app's Documents/game folder (Files app).
# =============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(SG_IOS_BUNDLE_ID "com.devz.sonicgenerations" CACHE STRING "iOS bundle identifier")
    set_target_properties(sonicgenerations PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${SG_IOS_BUNDLE_ID}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${SG_IOS_BUNDLE_ID}"
        # The SDK helper sets @executable_path, right for a flat macOS layout;
        # inside a bundle the dylibs live in Frameworks/.
        INSTALL_RPATH "@executable_path/Frameworks"
        BUILD_WITH_INSTALL_RPATH ON
    )
    # iOS only loads dylibs from inside the signed bundle.
    get_filename_component(SG_IOS_SDK_LIB "${rexglue_DIR}/../.." ABSOLUTE)
    set(SG_IOS_EMBED
        "${SG_IOS_SDK_LIB}/librexruntime.dylib"
        "${SG_IOS_SDK_LIB}/librexgpu-xenos.dylib"
        "${SG_IOS_SDK_LIB}/libMoltenVK.dylib")
    add_custom_target(sonicgenerations_ios_bundle ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:sonicgenerations>/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SG_IOS_EMBED}
                "$<TARGET_BUNDLE_CONTENT_DIR:sonicgenerations>/Frameworks"
        DEPENDS sonicgenerations ${SG_IOS_EMBED}
        COMMENT "Refreshing runtime dylibs in sonicgenerations.app/Frameworks"
        VERBATIM)

    # App icons. Without an asset catalog iOS resolves icons by file name from
    # the bundle root (CFBundleIcons in Info.plist), so the PNGs go in flat.
    file(GLOB SG_IOS_ICONS "${CMAKE_CURRENT_SOURCE_DIR}/ios/icons/*.png")
    if(SG_IOS_ICONS)
        add_custom_command(TARGET sonicgenerations_ios_bundle POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SG_IOS_ICONS}
                    "$<TARGET_BUNDLE_CONTENT_DIR:sonicgenerations>"
            COMMENT "Copying app icons into sonicgenerations.app"
            VERBATIM)
    endif()
endif()
