cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK ${CMAKE_CURRENT_LIST_DIR}/dependencies.lock)
# Only our component and what it requires (see firmware/CMakeLists.txt).
set(COMPONENTS src __pio_env)
project(widscan58-display)
