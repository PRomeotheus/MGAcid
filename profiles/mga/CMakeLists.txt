set(MGA_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
# Host code shared with the other profiles: nothing in it is specific to a game,
# so it sits beside the profiles rather than inside one. A profile opts in by
# listing the files it wants and adding this to its include path -- there is no
# library target, so a profile that wants none of it links nothing extra.
set(PSP_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")

# The AOT corpus is derived from the user's executable and is not committed.
# Until scripts/generate.sh has produced it, link an empty registry instead.
file(GLOB MGA_GENERATED CONFIGURE_DEPENDS "${MGA_PROFILE_DIR}/generated/*.cpp")
if(MGA_GENERATED)
    list(LENGTH MGA_GENERATED MGA_GENERATED_COUNT)
    message(STATUS "mga: ${MGA_GENERATED_COUNT} generated AOT units")
    if(MSVC)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            set(MGA_GENERATED_OPTIONS "/Od;/bigobj")
        else()
            set(MGA_GENERATED_OPTIONS "/O${PSPRECOMP_GENERATED_OPT_LEVEL};/Ob0;/bigobj")
        endif()
    else()
        set(MGA_GENERATED_OPTIONS "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0")
    endif()
    set_source_files_properties(${MGA_GENERATED} PROPERTIES COMPILE_OPTIONS "${MGA_GENERATED_OPTIONS}")
else()
    message(STATUS "mga: no generated AOT units; run profiles/mga/scripts/generate.sh")
    set(MGA_GENERATED host/generated_stub.cpp)
endif()

# Renderer: SDL3 for the window and Vulkan for drawing. Both are optional so the
# profile still builds headless on a machine without them.
option(MGA_RENDERER "Build the Vulkan renderer" ON)
set(MGA_RENDERER_SOURCES)
if(MGA_RENDERER)
    find_package(SDL3 QUIET)
    find_package(Vulkan QUIET)
    find_program(MGA_GLSLANG NAMES glslangValidator glslang)
    if(SDL3_FOUND AND Vulkan_FOUND AND MGA_GLSLANG)
        set(MGA_SHADER_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/ge_shaders.inc")
        add_custom_command(
            OUTPUT "${MGA_SHADER_INC}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
            COMMAND python3 "${MGA_PROFILE_DIR}/tools/embed_shaders.py" "${MGA_GLSLANG}" "${MGA_SHADER_INC}"
                    "kGeVertexShader=${MGA_PROFILE_DIR}/host/gpu/shaders/ge.vert"
                    "kGeFragmentShader=${MGA_PROFILE_DIR}/host/gpu/shaders/ge.frag"
                    "kPostVertexShader=${MGA_PROFILE_DIR}/host/gpu/shaders/post.vert"
                    "kPostFragmentShader=${MGA_PROFILE_DIR}/host/gpu/shaders/post.frag"
                    "kAoFragmentShader=${MGA_PROFILE_DIR}/host/gpu/shaders/ao.frag"
                    "kShadowVertexShader=${MGA_PROFILE_DIR}/host/gpu/shaders/shadow.vert"
            DEPENDS "${MGA_PROFILE_DIR}/host/gpu/shaders/ge.vert"
                    "${MGA_PROFILE_DIR}/host/gpu/shaders/ge.frag"
                    "${MGA_PROFILE_DIR}/host/gpu/shaders/post.vert"
                    "${MGA_PROFILE_DIR}/host/gpu/shaders/post.frag"
                    "${MGA_PROFILE_DIR}/host/gpu/shaders/ao.frag"
                    "${MGA_PROFILE_DIR}/host/gpu/shaders/shadow.vert"
                    "${MGA_PROFILE_DIR}/tools/embed_shaders.py"
            COMMENT "Compiling GE shaders to SPIR-V")
        add_custom_target(mga_shaders DEPENDS "${MGA_SHADER_INC}")
        # Dear ImGui (third_party/imgui, unmodified) draws the port's own
        # interface over the game.
        set(MGA_RENDERER_SOURCES
            host/gpu/frame_record.cpp
            host/gpu/vulkan_renderer.cpp
            host/ui/file_browser.cpp
            host/ui/font_menu.cpp
            host/ui/input_script.cpp
            host/ui/layer.cpp
            host/ui/menu.cpp
            host/ui/save_screen.cpp
            host/ui/savedata_dialog.cpp
            host/ui/setup_screens.cpp
            host/ui/text_input.cpp
            host/ui/widgets.cpp
            third_party/imgui/imgui.cpp
            third_party/imgui/imgui_draw.cpp
            third_party/imgui/imgui_tables.cpp
            third_party/imgui/imgui_widgets.cpp
            third_party/imgui/backends/imgui_impl_sdl3.cpp
            third_party/imgui/backends/imgui_impl_vulkan.cpp)
        message(STATUS "mga: Vulkan renderer enabled")
    else()
        message(STATUS "mga: renderer disabled (SDL3, Vulkan or glslang not found)")
        set(MGA_RENDERER OFF)
    endif()
endif()

# The build the in-game menu shows: `git describe` of the checkout, refreshed
# on every build but rewritten only when it changes, so only the menu
# recompiles after a commit.
set(MGA_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated_version/yakumo_version.hpp")
add_custom_target(mga_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${MGA_VERSION_HEADER}
            -P "${MGA_PROFILE_DIR}/tools/write_version.cmake"
    BYPRODUCTS "${MGA_VERSION_HEADER}"
    COMMENT "Checking the build version")

# FFmpeg decodes the game's music and its movies; by default it is built with
# the project (see cmake/FFmpeg.cmake).
include("${MGA_PROFILE_DIR}/cmake/FFmpeg.cmake")

# GE parsing, texture decoding and the audio mixer are backend independent and
# always built; only the device the sink opens depends on SDL.
set(MGA_HOST_SOURCES
    host/app_paths.cpp
    host/audio/atrac_decoder.cpp
    host/movie/avc_decoder.cpp
    host/movie/psmf_demuxer.cpp
    host/audio/audio_sink.cpp
    host/audio/sas_core.cpp
    host/gpu/ge_state.cpp
    host/gpu/ge_state_serialise.cpp
    host/gpu/texture_decode.cpp
    host/gpu/shadow_map.cpp
    "${PSP_COMMON_DIR}/texture_pack.cpp"
    "${PSP_COMMON_DIR}/texture_scale.cpp"
    host/perf/frame_stats.cpp
    host/perf/perf_overlay.cpp
    host/overlays.cpp
    host/mga_profile.cpp
    host/kernel/scene.cpp
    host/kernel/kernel.cpp
    host/kernel/kernel_state.cpp
    host/kernel/iso_image.cpp
    host/kernel/call_watch.cpp
    host/kernel/sound_paths.cpp
    host/kernel/module_loader.cpp
    host/hle/hle_common.cpp
    host/hle/hle_threadman.cpp
    host/hle/hle_sysmem.cpp
    host/hle/hle_io.cpp
    host/hle/hle_system.cpp
    host/hle/hle_media.cpp
    host/hle/hle_atrac.cpp
    host/hle/hle_mpeg.cpp
    host/fonts/game_font.cpp
    host/hle/hle_font.cpp
    host/hle/hle_utility.cpp
    host/hle/hle_savedata.cpp
    host/hle/hle_adhoc.cpp
    host/hle/hle_mga.cpp
    host/adhoc/client.cpp
    host/adhoc/discovery.cpp
    host/adhoc/host.cpp
    host/adhoc/server.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    host/install/executable_preparation.cpp
    host/install/front_end.cpp
    host/install/installer.cpp
    host/install/installer_dialogs.cpp
    host/install/user_data.cpp
    host/settings/settings.cpp
    host/state/save_state.cpp
    third_party/tiny_aes/aes.c
)
# tiny-AES-c is C; the project only enables C++, which compiles it unchanged.
set_source_files_properties(third_party/tiny_aes/aes.c PROPERTIES LANGUAGE CXX)

# The recompiled game code gets a target of its own. It needs only the
# psprecomp headers, so definitions and include paths the host adds to
# MGAcid (a renderer switch, a new library's headers) do not change its
# compile commands and rebuild all of it. Its objects still link straight into
# the executable, which exports their symbols to the overlay libraries.
add_library(mga_generated OBJECT ${MGA_GENERATED})
target_include_directories(mga_generated PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${MGA_PROFILE_DIR}/generated")
set_target_properties(mga_generated PROPERTIES JOB_POOL_COMPILE psprecomp_generated)

# Recompiled PRX libraries of the game (scripts/generate_modules.sh). Each
# modules/<prefix>/ joins a target of its own, so adding one never recompiles
# the executable's generated units, and module_corpora.cpp tells the
# module loader which corpus belongs to which module at which address. Like
# generated/, modules/ is derived from the user's disc and ignored by Git.
if(MSVC)
    if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
        set(MGA_MODULE_OPTIONS "/Od;/bigobj")
    else()
        set(MGA_MODULE_OPTIONS "/O${PSPRECOMP_GENERATED_OPT_LEVEL};/Ob0;/bigobj")
    endif()
else()
    set(MGA_MODULE_OPTIONS "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0")
endif()
file(GLOB MGA_MODULE_METADATA CONFIGURE_DEPENDS "${MGA_PROFILE_DIR}/modules/*/module.txt")
set(MGA_MODULE_SOURCES "")
set(MGA_MODULE_INCLUDES "")
set(MGA_MODULE_DECLARATIONS "")
set(MGA_MODULE_ENTRIES "")
foreach(meta IN LISTS MGA_MODULE_METADATA)
    get_filename_component(module_dir "${meta}" DIRECTORY)
    file(STRINGS "${meta}" module_lines)
    set(module_name "")
    set(module_base "")
    set(module_prefix "")
    set(module_hash "")
    foreach(line IN LISTS module_lines)
        if(line MATCHES "^(name|base|prefix|hash)=(.+)$")
            set(module_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    file(GLOB module_sources CONFIGURE_DEPENDS "${module_dir}/*.cpp")
    if(NOT module_name OR NOT module_base OR NOT module_prefix OR NOT module_hash OR NOT module_sources)
        message(WARNING "mga: incomplete module corpus in ${module_dir}")
        continue()
    endif()
    list(APPEND MGA_MODULE_SOURCES ${module_sources})
    list(APPEND MGA_MODULE_INCLUDES "${module_dir}")
    string(APPEND MGA_MODULE_DECLARATIONS "void register_${module_prefix}_functions(Runtime &runtime);\n")
    string(APPEND MGA_MODULE_ENTRIES
        "        {\"${module_name}\", ${module_base}u, ${module_hash}u, &psprecomp::register_${module_prefix}_functions},\n")
    message(STATUS "mga: recompiled module ${module_name} (${module_prefix}) at ${module_base}")
endforeach()
configure_file("${MGA_PROFILE_DIR}/host/module_corpora.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/module_corpora.cpp" @ONLY)
if(MGA_MODULE_SOURCES)
    add_library(mga_modules OBJECT ${MGA_MODULE_SOURCES})
    set_source_files_properties(${MGA_MODULE_SOURCES} PROPERTIES COMPILE_OPTIONS "${MGA_MODULE_OPTIONS}")
    target_include_directories(mga_modules PRIVATE "${PROJECT_SOURCE_DIR}/include" ${MGA_MODULE_INCLUDES})
    set_target_properties(mga_modules PROPERTIES JOB_POOL_COMPILE psprecomp_generated)
    set(MGA_MODULE_OBJECTS $<TARGET_OBJECTS:mga_modules>)
else()
    set(MGA_MODULE_OBJECTS "")
endif()

add_executable(MGAcid
    host/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/module_corpora.cpp"
    ${MGA_HOST_SOURCES}
    ${MGA_RENDERER_SOURCES}
    $<TARGET_OBJECTS:mga_generated>
    ${MGA_MODULE_OBJECTS}
)
if(WIN32)
    set(MGA_WINDOWS_ICON "${MGA_PROFILE_DIR}/packaging/windows/emblem.ico")
    configure_file(packaging/windows/Yakumo.rc.in Yakumo.rc @ONLY)
    target_sources(MGAcid PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Yakumo.rc")
endif()
if(MGA_RENDERER)
    add_dependencies(MGAcid mga_shaders mga_version)
    # The sink opens its own SDL audio subsystem, so it only needs SDL to exist.
    target_compile_definitions(MGAcid PRIVATE MGA_HAS_RENDERER=1 MGA_HAS_SDL_AUDIO=1 MGA_HAS_SDL=1)
    target_include_directories(MGAcid PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
        "${CMAKE_CURRENT_BINARY_DIR}/generated_version"
        "${MGA_PROFILE_DIR}/third_party/imgui")
    target_link_libraries(MGAcid PRIVATE SDL3::SDL3 Vulkan::Vulkan)
endif()
# The host only: the generated code never sees FFmpeg.
mga_use_ffmpeg(MGAcid)
target_include_directories(MGAcid PRIVATE
    "${MGA_PROFILE_DIR}/host"
    "${MGA_PROFILE_DIR}/third_party"
    "${MGA_PROFILE_DIR}/generated"
    # So the shared headers are reached as "common/...", the same spelling from
    # every profile.
    "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(MGAcid PRIVATE psprecomp_core ${CMAKE_DL_LIBS})
if(WIN32)
    # The ad hoc client's and server's sockets, and the list of interfaces.
    target_link_libraries(MGAcid PRIVATE ws2_32 iphlpapi)
endif()
find_package(Threads REQUIRED)
target_link_libraries(MGAcid PRIVATE Threads::Threads)
# The overlay libraries resolve psprecomp and runtime symbols against the
# executable that loads them, so it has to export them.
set_target_properties(MGAcid PROPERTIES
    ENABLE_EXPORTS TRUE
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
# The NID table is compiled in, so the program reads nothing from the checkout
# it was built in.
set(MGA_NIDS_CSV "${PROJECT_SOURCE_DIR}/configs/nids.csv")
set(MGA_NIDS_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_nids/nid_table.inc")
add_custom_command(
    OUTPUT "${MGA_NIDS_INC}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${MGA_NIDS_CSV} -DOUTPUT=${MGA_NIDS_INC}
            -P "${MGA_PROFILE_DIR}/tools/embed_nids.cmake"
    DEPENDS "${MGA_NIDS_CSV}" "${MGA_PROFILE_DIR}/tools/embed_nids.cmake"
    COMMENT "Embedding the NID table")
target_sources(MGAcid PRIVATE "${MGA_NIDS_INC}")
target_include_directories(MGAcid PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_nids")

# A release build runs from wherever it is unpacked: it has no fallback to
# this checkout's game directory, and on Linux it finds the libraries it ships
# in lib/ next to the executable and nothing else.
option(MGA_RELEASE "Build for distribution: no checkout paths, libraries from lib/ next to the executable" OFF)
if(NOT MGA_RELEASE)
    # The checkout's profiles/mga/game, a developer fallback after the
    # per-user data directory. Only main.cpp reads it; on the target it would be
    # on every generated unit's command line, so moving the checkout would
    # rebuild all of them and no other checkout could reuse their cache entries.
    set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        MGA_DEFAULT_GAME_DIR="${MGA_PROFILE_DIR}/game")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(MGAcid PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib")
    # The C++ runtime is linked in statically, so a build made against an old
    # glibc runs wherever that glibc does, and its symbols stay private: the
    # executable exports its own symbols to the overlay libraries, and a
    # second, exported libstdc++ would clash with the system's one that the
    # graphics driver loads.
    set(MGA_RELEASE_LINK_OPTIONS -static-libstdc++ "LINKER:--exclude-libs,libstdc++.a:libsupc++.a")
    target_link_options(MGAcid PRIVATE ${MGA_RELEASE_LINK_OPTIONS})
endif()
set_target_properties(MGAcid PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    JOB_POOL_COMPILE psprecomp_generated)

# Save-data checks that need no game data: crypto test vectors and round trips.
add_executable(mga_savedata_tests
    tests/savedata_tests.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    third_party/tiny_aes/aes.c)
target_include_directories(mga_savedata_tests PRIVATE
    "${MGA_PROFILE_DIR}/host"
    "${MGA_PROFILE_DIR}/third_party")
set_target_properties(mga_savedata_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mga_savedata_tests COMMAND mga_savedata_tests)
endif()

# The built-in ad hoc server against the client and a raw protocol peer, and
# local network discovery, all over the loopback address.
add_executable(mga_adhoc_tests
    tests/adhoc_tests.cpp
    host/adhoc/client.cpp
    host/adhoc/discovery.cpp
    host/adhoc/server.cpp)
target_include_directories(mga_adhoc_tests PRIVATE "${MGA_PROFILE_DIR}/host")
target_link_libraries(mga_adhoc_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(mga_adhoc_tests PRIVATE ws2_32 iphlpapi)
endif()
set_target_properties(mga_adhoc_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mga_adhoc_tests COMMAND mga_adhoc_tests)
endif()

# Save states with no game: that every field written comes back, by writing a
# state, reading it, writing it again and comparing the two streams -- which is
# what catches a field that was added to a structure and serialised in only one
# direction. The one module-loader symbol the kernel refers to is stubbed rather
# than linked, since none of this reaches it.
add_executable(mga_state_tests
    tests/state_tests.cpp
    tests/state_stubs.cpp
    host/gpu/ge_state.cpp
    host/gpu/ge_state_serialise.cpp
    host/kernel/kernel.cpp
    host/kernel/kernel_state.cpp
    host/perf/frame_stats.cpp
    host/settings/settings.cpp
    host/install/user_data.cpp
    host/app_paths.cpp)
target_include_directories(mga_state_tests PRIVATE "${MGA_PROFILE_DIR}/host")
target_link_libraries(mga_state_tests PRIVATE psprecomp_core)
if(TARGET SDL3::SDL3)
    target_link_libraries(mga_state_tests PRIVATE SDL3::SDL3)
endif()
set_target_properties(mga_state_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mga_state_tests COMMAND mga_state_tests)
endif()

# Replacement textures: that a dump reads back exactly, and that every PNG
# layout an image editor produces is understood. Needs the PNG codec, so it is
# only built when FFmpeg is there to provide one.
if(MGA_HAS_FFMPEG)
    add_executable(mga_texture_pack_tests
        tests/texture_pack_tests.cpp
        "${PSP_COMMON_DIR}/texture_pack.cpp")
    target_include_directories(mga_texture_pack_tests PRIVATE
        "${MGA_PROFILE_DIR}/host"
        "${CMAKE_CURRENT_SOURCE_DIR}/..")
    mga_use_ffmpeg(mga_texture_pack_tests)
    set_target_properties(mga_texture_pack_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME mga_texture_pack_tests COMMAND mga_texture_pack_tests)
    endif()
endif()

# Chained AOT calls nest native frames; give the guest-executing main thread
# the same 64 MiB stack the Windows profiles use.
if(MSVC)
    target_link_options(MGAcid PRIVATE /STACK:67108864)
elseif(APPLE)
    target_link_options(MGAcid PRIVATE "LINKER:-stack_size,0x4000000")
endif()

# Recompiled overlay corpora, produced by tools/add_overlay.py. Each directory
# under overlays/ holds one corpus and becomes one shared module that the host
# loads at run time, so a new overlay never relinks the executable.
set(MGA_OVERLAY_DIR "${CMAKE_BINARY_DIR}/bin/overlays")

function(mga_add_overlay meta_path)
    get_filename_component(source_dir "${meta_path}" DIRECTORY)
    get_filename_component(MGA_OVERLAY_PREFIX "${source_dir}" NAME)
    file(STRINGS "${meta_path}" meta_lines REGEX "^(base|size|code_size|hash|name)=")
    foreach(line IN LISTS meta_lines)
        string(REGEX MATCH "^([a-z_]+)=(.+)$" matched "${line}")
        if(NOT matched)
            continue()
        endif()
        string(TOUPPER "${CMAKE_MATCH_1}" key)
        set(MGA_OVERLAY_${key} "${CMAKE_MATCH_2}")
    endforeach()
    if(NOT MGA_OVERLAY_BASE OR NOT MGA_OVERLAY_SIZE OR NOT MGA_OVERLAY_CODE_SIZE
       OR NOT MGA_OVERLAY_HASH OR NOT MGA_OVERLAY_NAME)
        message(WARNING "mga: incomplete overlay metadata in ${meta_path}")
        return()
    endif()

    file(GLOB sources CONFIGURE_DEPENDS "${source_dir}/*.cpp")
    if(NOT sources)
        message(WARNING "mga: no sources for overlay ${MGA_OVERLAY_PREFIX}")
        return()
    endif()
    set(entry_point "${CMAKE_CURRENT_BINARY_DIR}/overlays/${MGA_OVERLAY_PREFIX}_module.cpp")
    configure_file("${MGA_PROFILE_DIR}/host/overlay_module.cpp.in" "${entry_point}" @ONLY)

    set(target "overlay_${MGA_OVERLAY_PREFIX}")
    add_library(${target} MODULE ${sources} "${entry_point}")
    # Headers only: linking psprecomp_core would give the module its own copy of
    # the runtime state the host already owns.
    target_include_directories(${target} PRIVATE
        "${source_dir}" "${MGA_PROFILE_DIR}/host" "${PROJECT_SOURCE_DIR}/include")
    if(WIN32 OR APPLE)
        # Both linkers want the host binary while linking the module: MSVC for
        # its import library, ld64 as the bundle loader. ELF leaves the host
        # symbols undefined and resolves them when the module is loaded.
        target_link_libraries(${target} PRIVATE MGAcid)
    else()
        add_dependencies(${target} MGAcid)
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE PSPRECOMP_IMPORT_HOST_SYMBOLS=1)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            target_compile_options(${target} PRIVATE /Od /bigobj)
        else()
            target_compile_options(${target} PRIVATE /O${PSPRECOMP_GENERATED_OPT_LEVEL} /Ob0 /bigobj)
        endif()
    else()
        target_compile_options(${target} PRIVATE -O${PSPRECOMP_GENERATED_OPT_LEVEL} -g0)
    endif()
    if(MGA_RELEASE_LINK_OPTIONS)
        target_link_options(${target} PRIVATE ${MGA_RELEASE_LINK_OPTIONS})
    endif()
    set_target_properties(${target} PROPERTIES
        JOB_POOL_COMPILE psprecomp_generated
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
        OUTPUT_NAME "${MGA_OVERLAY_PREFIX}"
        LIBRARY_OUTPUT_DIRECTORY "${MGA_OVERLAY_DIR}")
endfunction()

file(GLOB MGA_OVERLAY_METADATA CONFIGURE_DEPENDS "${MGA_PROFILE_DIR}/overlays/*/meta.txt")
foreach(meta IN LISTS MGA_OVERLAY_METADATA)
    mga_add_overlay("${meta}")
endforeach()
list(LENGTH MGA_OVERLAY_METADATA MGA_OVERLAY_COUNT)
message(STATUS "mga: ${MGA_OVERLAY_COUNT} overlay librar(ies) in ${MGA_OVERLAY_DIR}")
