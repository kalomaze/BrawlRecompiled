cmake_minimum_required(VERSION 3.25)
# Run once: extracts the whole disc to Disc/DATA (or -DGAME=<folder>), which build.cmake builds from and the game plays.
if(NOT DEFINED DISC OR NOT EXISTS "${DISC}")
    message(FATAL_ERROR "Usage: cmake -DDISC=/path/to/brawl.iso -P extract.cmake")
endif()
set(EXTRACT ON)
include("${CMAKE_CURRENT_LIST_DIR}/build.cmake")
