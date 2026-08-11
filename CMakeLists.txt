# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_board_bringup)

# Strict warnings are applied to the `main` component only (see
# main/CMakeLists.txt). Setting them here with idf_build_set_property() would
# also apply them to ESP-IDF and to managed components, which do not build
# warning-clean and would break the build.
