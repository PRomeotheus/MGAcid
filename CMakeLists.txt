cmake_minimum_required(VERSION 3.20)
project(MGAcid LANGUAGES CXX)

# The PSPRecomp framework this game is built on. In the PSPRecomp workspace
# that is the live checkout at ../../framework, so a change there reaches this
# game on its next build; in a standalone clone of this repository it is the
# pinned submodule in external/PSPRecomp (git submodule update --init).
# -DPSPRECOMP_DIR=<path> names another one.
set(PSPRECOMP_DIR "" CACHE PATH "PSPRecomp framework checkout (empty: find it)")
if(NOT PSPRECOMP_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../framework/cmake/PSPRecomp.cmake")
        set(PSPRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../framework")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/PSPRecomp/cmake/PSPRecomp.cmake")
        set(PSPRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/PSPRecomp")
    else()
        message(FATAL_ERROR "The PSPRecomp framework was not found. In a clone of this repository run\n"
                            "  git submodule update --init\n"
                            "or pass -DPSPRECOMP_DIR=<framework checkout>.")
    endif()
endif()
get_filename_component(PSPRECOMP_DIR "${PSPRECOMP_DIR}" ABSOLUTE)
message(STATUS "PSPRecomp framework: ${PSPRECOMP_DIR}")
include("${PSPRECOMP_DIR}/cmake/PSPRecomp.cmake")

psprecomp_add_game(
    NAME            MGAcid
    ENV_PREFIX      MGA
    TITLE           "Metal Gear Ac!d"
    DISC_ID         ULUS10006
    DISC_ID_DISPLAY ULUS-10006
    DATA_ORG        MGAcid
    DATA_APP        MGA
    STATE_EXT       mgastate
    RC_TEMPLATE     packaging/windows/MGAcid.rc.in
    SOURCES
        host/kernel/sound_paths.cpp
        host/kernel/module_sweep.cpp)
