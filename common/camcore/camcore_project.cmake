# Included by each app's top-level CMakeLists.txt, after project.cmake and
# before project():
#
#     include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#     include(${CMAKE_CURRENT_LIST_DIR}/../../common/camcore/camcore_project.cmake)
#     project(<app>)
#
# It adds the camcore component and sets the build-wide properties the camera
# needs, which a component cannot set for itself.

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}")

# The apps share one copy of the tuning, ipa/imx708_control.json next to this
# file. It is imx708_default.json with two changes:
#
#  - The AGC hold band (agc.luma_adjust target_low/target/target_high) moved
#    from 50/100/150 to 52/60/68. Inside the band AE changes nothing, and
#    50..150 is about 1.6 stops wide, so where AE came to rest depended on
#    which side it approached from: Y=133 after boot (from dark), Y=188 after
#    returning from a bright manual exposure. Manual -> auto is a routine move
#    here, so auto has to land in the same place every time.
#
#  - No "awb" block. esp_ipa has no runtime switch for AWB, and while it runs
#    it overwrites any white balance the application sets; camctl.c does white
#    balance itself (auto, one-push and manual). The stock block was also
#    barely acting - min_red/blue_gain_step 0.5 against the README's 0.034 -
#    which is the likely source of the green cast. "acc" has a single CT-0
#    identity CCM, so nothing else depended on AWB's colour temperature.
set(imx708_ipa_json "${CMAKE_CURRENT_LIST_DIR}/ipa/imx708_control.json")

# Hand the IMX708's ISP tuning config to esp_ipa. This has to sit between
# project.cmake and project(): esp_ipa reads the property while its own
# CMakeLists is processed, which is too late for a component to set it.
#
# Only the string is needed now - esp_ipa resolves it against the project
# directory and checks it exists when *it* is configured, by which time the
# component manager has already populated managed_components/. So the registry
# path above is valid even on a clean first build.
#
# Get it wrong and nothing complains: esp_ipa's existence check calls
# message(FETAL_ERROR ...), which is not a real CMake mode, so a missing file
# prints a line and the build carries on to produce a stream with no
# auto-exposure and no white balance. camcore/CMakeLists.txt re-checks the
# resolved path and fails the build instead.
idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${imx708_ipa_json}" APPEND)
idf_build_set_property(IMX708_IPA_JSON_RESOLVED "${imx708_ipa_json}")

# ---------------------------------------------------------------------------
# ABI fix: make esp_ipa_stats_t match the prebuilt esp_ipa binary.
#
# esp_ipa ships as a closed .a compiled with the AWB sub-window grid present,
# i.e. with ISP_AWB_WINDOW_X_NUM / _Y_NUM defined as 5. Those macros only exist
# in ESP-IDF >= v5.4.4. Below that they are undefined, so esp_ipa_types.h
# collapses esp_ipa_stats_t::awb_subwin to a [0][0] array - 400 bytes shorter
# than the library expects, which puts af_stats at offset 196 in a 224-byte
# struct while the library reads it from 596. Autofocus then scores unrelated
# heap memory and always settles on its first scan point.
#
# This has to be global - esp_video writes the struct, the generated IPA config
# declares it, and the library reads it, so all three must agree. From v5.4.4
# ESP-IDF defines the macros itself and the guard below steps aside.
if(IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 4 AND IDF_VERSION_PATCH LESS 4)
    idf_build_set_property(COMPILE_DEFINITIONS "ISP_AWB_WINDOW_X_NUM=5" APPEND)
    idf_build_set_property(COMPILE_DEFINITIONS "ISP_AWB_WINDOW_Y_NUM=5" APPEND)
endif()

