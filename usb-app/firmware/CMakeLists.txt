# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

# camcore's sdkconfig first (camera, PSRAM, flash), then this app's.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../../common/camcore/sdkconfig.camcore"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The camera, the light board, the button and the HTTP API (common/camcore).
include(${CMAKE_CURRENT_LIST_DIR}/../../common/camcore/camcore_project.cmake)

set(PROJECT_VER "0.1.0")

project(usbapp)
