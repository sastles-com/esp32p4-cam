# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The camera, the light board, the button and the HTTP API live in
# common/camcore, shared with usb-app. This adds the component and sets the
# build-wide properties it needs (ISP tuning, ABI fix); see the file.
include(${CMAKE_CURRENT_LIST_DIR}/../../common/camcore/camcore_project.cmake)

# Without this, IDF takes the version from `git describe`, and a repository
# with no commits yet makes that a configure error rather than a fallback.
set(PROJECT_VER "0.3.0")

project(wifiapp)
