# FFmpeg decodes the game's music (ATRAC3, ATRAC3plus) and its movies (H.264).
# Only libavcodec and libavutil are used, always as shared libraries.
#
# MGA_FFMPEG selects where they come from:
#   bundled  (default) On macOS and Linux, download the pinned FFmpeg release
#            below, check its SHA-256 and build it at configure time with the
#            LGPL-only configuration below. On Windows, whose documented MSVC
#            toolchain cannot run FFmpeg's configure, download the pinned
#            prebuilt LGPL shared build below instead. Either way the libraries
#            end up next to the executable, so it needs no FFmpeg on the system.
#   system   The libavcodec and libavutil that pkg-config finds.
#   OFF      No FFmpeg: the game has no music and skips its movies.
#
# Sets MGA_HAS_FFMPEG and defines mga_use_ffmpeg(<target>).

# FFmpeg 7.1.5 (libavcodec 61, libavutil 59), the release the Linux packages use.
set(MGA_FFMPEG_VERSION 7.1.5)
set(MGA_FFMPEG_URL "https://ffmpeg.org/releases/ffmpeg-${MGA_FFMPEG_VERSION}.tar.xz")
set(MGA_FFMPEG_SHA256 de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f)
set(MGA_FFMPEG_SOVERSIONS avcodec 61 avutil 59)

# Only what the game needs: the ATRAC3, ATRAC3plus and H.264 decoders. The host
# splits the movie streams into access units and converts pixels itself, so no
# demuxer, parser, scaler or resampler is built. No GPL or non-free parts: the
# result is LGPL-2.1-or-later, which the build checks.
set(MGA_FFMPEG_CONFIGURE_FLAGS
    --enable-shared
    --disable-static
    --disable-programs
    --disable-doc
    --disable-avdevice
    --disable-avformat
    --disable-avfilter
    --disable-swscale
    --disable-swresample
    --disable-network
    --disable-autodetect
    --disable-everything
    --enable-decoder=atrac3,atrac3p,h264
    --disable-x86asm
    --disable-debug)

# Windows x64: BtbN/FFmpeg-Builds, the LGPL shared build of the 7.1.5 release
# branch from the monthly build of June 2026, which that project keeps for two
# years. It is built with --enable-version3 and without --enable-gpl or
# --enable-nonfree, so it is LGPL-3.0-or-later. It depends on libswresample.
set(MGA_FFMPEG_WINDOWS_URL
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-06-30-13-34/ffmpeg-n7.1.5-1-g7d0e842004-win64-lgpl-shared-7.1.zip")
set(MGA_FFMPEG_WINDOWS_SHA256 03a8003e245c08df4277d7b0adc50b93a97ddd4a3aaafea21943c4384df59895)
set(MGA_FFMPEG_WINDOWS_DLLS avcodec-61 avutil-59 swresample-5)

set(MGA_FFMPEG "bundled" CACHE STRING
    "Where FFmpeg comes from: bundled (built with the project), system (pkg-config) or OFF (no music, no movies)")
set_property(CACHE MGA_FFMPEG PROPERTY STRINGS bundled system OFF)
# MGA_FFMPEG used to be ON/OFF: ON (the old default) becomes bundled.
string(TOUPPER "${MGA_FFMPEG}" _mga_ffmpeg_mode)
if(_mga_ffmpeg_mode MATCHES "^(ON|TRUE|YES|Y|1|BUNDLED)$")
    set(_mga_ffmpeg_mode bundled)
elseif(_mga_ffmpeg_mode MATCHES "^(OFF|FALSE|NO|N|0)$")
    set(_mga_ffmpeg_mode OFF)
elseif(_mga_ffmpeg_mode STREQUAL "SYSTEM")
    set(_mga_ffmpeg_mode system)
else()
    message(FATAL_ERROR "MGA_FFMPEG must be bundled, system or OFF, not '${MGA_FFMPEG}'")
endif()
if(NOT MGA_FFMPEG STREQUAL _mga_ffmpeg_mode)
    set_property(CACHE MGA_FFMPEG PROPERTY TYPE STRING)
    set_property(CACHE MGA_FFMPEG PROPERTY VALUE "${_mga_ffmpeg_mode}")
endif()

set(MGA_FFMPEG_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/downloads" CACHE PATH
    "Where the bundled FFmpeg archive is downloaded to, or found without downloading")

set(MGA_HAS_FFMPEG OFF)
set(_mga_ffmpeg_root "${CMAKE_BINARY_DIR}/_deps/ffmpeg")
set(_mga_ffmpeg_bin "${CMAKE_BINARY_DIR}/bin")

# Downloads <url> to <file name> in MGA_FFMPEG_DOWNLOAD_DIR unless a copy
# with the pinned SHA-256 is already there; sets <out> to its path.
function(_mga_ffmpeg_fetch out name url sha256)
    set(archive "${MGA_FFMPEG_DOWNLOAD_DIR}/${name}")
    if(EXISTS "${archive}")
        file(SHA256 "${archive}" existing)
        if(existing STREQUAL sha256)
            set(${out} "${archive}" PARENT_SCOPE)
            return()
        endif()
    endif()
    message(STATUS "mga: downloading ${url}")
    file(DOWNLOAD "${url}" "${archive}.part" STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${archive}.part")
        message(FATAL_ERROR "mga: could not download ${url}: ${status}\n"
            "Put the file in ${MGA_FFMPEG_DOWNLOAD_DIR} by hand, or configure with "
            "-DMGA_FFMPEG=system to use an installed FFmpeg.")
    endif()
    file(SHA256 "${archive}.part" actual)
    if(NOT actual STREQUAL sha256)
        file(REMOVE "${archive}.part")
        message(FATAL_ERROR "mga: ${url} has SHA-256 ${actual}, expected ${sha256}")
    endif()
    file(RENAME "${archive}.part" "${archive}")
    set(${out} "${archive}" PARENT_SCOPE)
endfunction()

# Runs a step of the FFmpeg build with its output in <log>; stops with the end
# of the log on failure.
function(_mga_ffmpeg_run what log)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${_mga_ffmpeg_root}/build"
        OUTPUT_FILE "${log}" ERROR_FILE "${log}.err"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        file(READ "${log}.err" errors)
        message(FATAL_ERROR "mga: FFmpeg ${what} failed (${result}); see ${log} and ${log}.err\n${errors}")
    endif()
endfunction()

# A short note next to the libraries: what they are and where their source is.
function(_mga_ffmpeg_write_notice dir)
    string(CONCAT text ${ARGN})
    file(WRITE "${dir}/FFmpeg-SOURCE.txt" "${text}")
endfunction()

if(_mga_ffmpeg_mode STREQUAL "OFF")
    message(WARNING
        "MGA_FFMPEG=OFF: the game will have NO MUSIC and will SKIP ITS MOVIES. "
        "Configure with -DMGA_FFMPEG=bundled (the default) to build FFmpeg with the project.")

elseif(_mga_ffmpeg_mode STREQUAL "system")
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(MGA_LIBAV QUIET IMPORTED_TARGET libavcodec libavutil)
    endif()
    if(NOT MGA_LIBAV_FOUND)
        message(FATAL_ERROR
            "MGA_FFMPEG=system, but pkg-config finds no libavcodec and libavutil. Install "
            "FFmpeg's development files, or configure with -DMGA_FFMPEG=bundled (the default).")
    endif()
    add_library(mga_ffmpeg INTERFACE)
    target_link_libraries(mga_ffmpeg INTERFACE PkgConfig::MGA_LIBAV)
    set(MGA_HAS_FFMPEG ON)
    message(STATUS "mga: FFmpeg libavcodec ${MGA_LIBAV_libavcodec_VERSION} from the system; music and movies enabled")

elseif(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm|aarch")
        message(FATAL_ERROR "mga: the bundled FFmpeg for Windows is x64 only. "
            "Configure with -DMGA_FFMPEG=system and an FFmpeg of your own.")
    endif()
    string(REGEX MATCH "[^/]+\\.zip$" _mga_ffmpeg_zip "${MGA_FFMPEG_WINDOWS_URL}")
    string(REGEX REPLACE "\\.zip$" "" _mga_ffmpeg_name "${_mga_ffmpeg_zip}")
    set(_mga_ffmpeg_prefix "${_mga_ffmpeg_root}/${_mga_ffmpeg_name}")
    if(NOT EXISTS "${_mga_ffmpeg_prefix}/yakumo.stamp")
        _mga_ffmpeg_fetch(_mga_ffmpeg_archive "${_mga_ffmpeg_zip}"
            "${MGA_FFMPEG_WINDOWS_URL}" "${MGA_FFMPEG_WINDOWS_SHA256}")
        file(REMOVE_RECURSE "${_mga_ffmpeg_prefix}")
        file(ARCHIVE_EXTRACT INPUT "${_mga_ffmpeg_archive}" DESTINATION "${_mga_ffmpeg_root}")
        file(WRITE "${_mga_ffmpeg_prefix}/yakumo.stamp" "${MGA_FFMPEG_WINDOWS_SHA256}\n")
    endif()
    set(_mga_ffmpeg_includes "${_mga_ffmpeg_prefix}/include")
    foreach(dll IN LISTS MGA_FFMPEG_WINDOWS_DLLS)
        configure_file("${_mga_ffmpeg_prefix}/bin/${dll}.dll" "${_mga_ffmpeg_bin}/${dll}.dll" COPYONLY)
    endforeach()
    configure_file("${_mga_ffmpeg_prefix}/LICENSE.txt" "${_mga_ffmpeg_bin}/FFmpeg-LICENSE.txt" COPYONLY)
    _mga_ffmpeg_write_notice("${_mga_ffmpeg_bin}"
        "The FFmpeg libraries here (avcodec, avutil, swresample) are an unmodified prebuilt\n"
        "LGPL shared build of FFmpeg ${MGA_FFMPEG_VERSION} from BtbN/FFmpeg-Builds, licensed under the\n"
        "GNU LGPL version 3 or later (FFmpeg-LICENSE.txt). They are dynamically linked.\n\n"
        "Build:  ${MGA_FFMPEG_WINDOWS_URL}\n"
        "Source: https://github.com/FFmpeg/FFmpeg/tree/release/7.1 (the commit is in the file name)\n"
        "Build scripts: https://github.com/BtbN/FFmpeg-Builds\n")
    add_library(mga_ffmpeg INTERFACE)
    foreach(lib avcodec avutil)
        add_library(mga_ffmpeg_${lib} SHARED IMPORTED)
        foreach(dll IN LISTS MGA_FFMPEG_WINDOWS_DLLS)
            if(dll MATCHES "^${lib}-")
                set(_mga_ffmpeg_dll "${dll}")
            endif()
        endforeach()
        set_target_properties(mga_ffmpeg_${lib} PROPERTIES
            IMPORTED_LOCATION "${_mga_ffmpeg_bin}/${_mga_ffmpeg_dll}.dll"
            IMPORTED_IMPLIB "${_mga_ffmpeg_prefix}/lib/${lib}.lib")
        target_link_libraries(mga_ffmpeg INTERFACE mga_ffmpeg_${lib})
    endforeach()
    target_include_directories(mga_ffmpeg INTERFACE "${_mga_ffmpeg_includes}")
    set(MGA_HAS_FFMPEG ON)
    message(STATUS "mga: bundled FFmpeg ${MGA_FFMPEG_VERSION} (prebuilt, LGPL); music and movies enabled")

else()
    find_program(MGA_MAKE NAMES gmake make)
    if(NOT MGA_MAKE)
        message(FATAL_ERROR "mga: building the bundled FFmpeg needs make. Install it, or "
            "configure with -DMGA_FFMPEG=system to use an installed FFmpeg.")
    endif()
    set(_mga_ffmpeg_src "${_mga_ffmpeg_root}/ffmpeg-${MGA_FFMPEG_VERSION}")
    set(_mga_ffmpeg_prefix "${_mga_ffmpeg_root}/install")
    # Platform settings that do not change what is built: where macOS finds the
    # libraries (the executable's rpath) and the compiler to use.
    set(_mga_ffmpeg_flags ${MGA_FFMPEG_CONFIGURE_FLAGS})
    if(APPLE)
        list(APPEND _mga_ffmpeg_flags --install-name-dir=@rpath)
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            list(APPEND _mga_ffmpeg_flags
                "--extra-cflags=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
                "--extra-ldflags=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
    endif()
    if(DEFINED ENV{CC})
        list(APPEND _mga_ffmpeg_flags "--cc=$ENV{CC}")
    endif()
    # Built once per build directory; again only when the pin or the flags change.
    string(REPLACE ";" " " _mga_ffmpeg_stamp
        "${MGA_FFMPEG_VERSION} ${MGA_FFMPEG_SHA256} ${_mga_ffmpeg_flags}")
    set(_mga_ffmpeg_stamp_file "${_mga_ffmpeg_prefix}/yakumo.stamp")
    set(_mga_ffmpeg_built "")
    if(EXISTS "${_mga_ffmpeg_stamp_file}")
        file(READ "${_mga_ffmpeg_stamp_file}" _mga_ffmpeg_built)
    endif()
    if(NOT _mga_ffmpeg_built STREQUAL _mga_ffmpeg_stamp)
        _mga_ffmpeg_fetch(_mga_ffmpeg_archive "ffmpeg-${MGA_FFMPEG_VERSION}.tar.xz"
            "${MGA_FFMPEG_URL}" "${MGA_FFMPEG_SHA256}")
        message(STATUS "mga: building FFmpeg ${MGA_FFMPEG_VERSION} "
            "(${PSPRECOMP_GENERATED_JOBS} jobs, once per build directory)")
        file(REMOVE_RECURSE "${_mga_ffmpeg_src}" "${_mga_ffmpeg_root}/build" "${_mga_ffmpeg_prefix}")
        file(ARCHIVE_EXTRACT INPUT "${_mga_ffmpeg_archive}" DESTINATION "${_mga_ffmpeg_root}")
        file(MAKE_DIRECTORY "${_mga_ffmpeg_root}/build")
        set(_log "${_mga_ffmpeg_root}/build")
        _mga_ffmpeg_run(configure "${_log}/configure.log"
            sh "${_mga_ffmpeg_src}/configure" "--prefix=${_mga_ffmpeg_prefix}" ${_mga_ffmpeg_flags})
        # The licensing in docs/SOURCE_PROVENANCE.md assumes exactly this.
        file(READ "${_log}/configure.log" _mga_ffmpeg_configure)
        file(READ "${_log}/config.h" _mga_ffmpeg_config)
        if(NOT _mga_ffmpeg_configure MATCHES "(^|\n)License: LGPL version 2\\.1 or later\n"
           OR NOT _mga_ffmpeg_config MATCHES "\n#define CONFIG_GPL 0\n"
           OR NOT _mga_ffmpeg_config MATCHES "\n#define CONFIG_NONFREE 0\n")
            message(FATAL_ERROR "mga: the FFmpeg configuration is not LGPL-2.1-or-later only; "
                "see ${_log}/configure.log")
        endif()
        _mga_ffmpeg_run(build "${_log}/build.log" "${MGA_MAKE}" -j${PSPRECOMP_GENERATED_JOBS})
        _mga_ffmpeg_run(install "${_log}/install.log" "${MGA_MAKE}" install)
        file(WRITE "${_mga_ffmpeg_stamp_file}" "${_mga_ffmpeg_stamp}")
    endif()

    # The libraries go to lib/ next to the executable, under the names the
    # loader looks for; the executable's rpath points there.
    set(_mga_ffmpeg_lib "${_mga_ffmpeg_bin}/lib")
    add_library(mga_ffmpeg INTERFACE)
    set(_versions ${MGA_FFMPEG_SOVERSIONS})
    while(_versions)
        list(POP_FRONT _versions lib soversion)
        if(APPLE)
            set(runtime_name "lib${lib}.${soversion}.dylib")
        else()
            set(runtime_name "lib${lib}.so.${soversion}")
        endif()
        configure_file("${_mga_ffmpeg_prefix}/lib/${runtime_name}" "${_mga_ffmpeg_lib}/${runtime_name}" COPYONLY)
        add_library(mga_ffmpeg_${lib} SHARED IMPORTED)
        set_target_properties(mga_ffmpeg_${lib} PROPERTIES
            IMPORTED_LOCATION "${_mga_ffmpeg_lib}/${runtime_name}")
        if(NOT APPLE)
            set_target_properties(mga_ffmpeg_${lib} PROPERTIES IMPORTED_SONAME "${runtime_name}")
        endif()
        target_link_libraries(mga_ffmpeg INTERFACE mga_ffmpeg_${lib})
    endwhile()
    target_include_directories(mga_ffmpeg INTERFACE "${_mga_ffmpeg_prefix}/include")
    configure_file("${_mga_ffmpeg_src}/COPYING.LGPLv2.1" "${_mga_ffmpeg_lib}/FFmpeg-COPYING.LGPLv2.1.txt" COPYONLY)
    string(REPLACE ";" " " _mga_ffmpeg_flag_text "${MGA_FFMPEG_CONFIGURE_FLAGS}")
    _mga_ffmpeg_write_notice("${_mga_ffmpeg_lib}"
        "The FFmpeg libraries here (libavcodec, libavutil) are FFmpeg ${MGA_FFMPEG_VERSION},\n"
        "unmodified, licensed under the GNU LGPL version 2.1 or later\n"
        "(FFmpeg-COPYING.LGPLv2.1.txt). They are dynamically linked.\n\n"
        "Source: ${MGA_FFMPEG_URL}\n"
        "SHA-256: ${MGA_FFMPEG_SHA256}\n"
        "Configured with: ${_mga_ffmpeg_flag_text}\n")
    set(MGA_FFMPEG_RPATH "$ORIGIN/lib")
    if(APPLE)
        set(MGA_FFMPEG_RPATH "@loader_path/lib")
    endif()
    set(MGA_HAS_FFMPEG ON)
    message(STATUS "mga: bundled FFmpeg ${MGA_FFMPEG_VERSION} (LGPL, atrac3 atrac3p h264); music and movies enabled")
endif()

# Links <target> against FFmpeg and lets it find the bundled libraries.
function(mga_use_ffmpeg target)
    if(NOT MGA_HAS_FFMPEG)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE MGA_HAS_FFMPEG=1)
    target_link_libraries(${target} PRIVATE mga_ffmpeg)
    if(MGA_FFMPEG_RPATH)
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${MGA_FFMPEG_RPATH}")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "${MGA_FFMPEG_RPATH}")
    endif()
endfunction()
