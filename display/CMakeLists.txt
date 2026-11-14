cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One component lock per target (as firmware/): the component manager
# rewrites the lock's target field on every build.
idf_build_set_property(DEPENDENCIES_LOCK ${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET})
# Only our component and what it requires (see firmware/CMakeLists.txt).
set(COMPONENTS src __pio_env)
project(widscan58-display)
