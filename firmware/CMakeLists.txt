cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One component lock per target: the component manager rewrites the lock's
# target field on every build, so a shared file would dirty the tree (and
# PROJECT_VER) whenever the other target is built.
idf_build_set_property(DEPENDENCIES_LOCK ${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET})
# The app version INFO reports, and the one the web flasher's manifest carries
# (.github/workflows/pages.yml runs the same git describe). Set at configure
# time, so a local incremental build can lag behind HEAD.
execute_process(COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PROJECT_VER)
    set(PROJECT_VER "unknown")
endif()
project(widscan58)
