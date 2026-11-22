cmake_minimum_required(VERSION 3.21)
project(acesms_ymfm_example LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This FM DLL example requires Windows x64")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ACESMS_PACKAGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_library(ymfm_example STATIC
    "${ACESMS_PACKAGE_ROOT}/third_party/ymfm/src/ymfm_opl.cpp"
    "${ACESMS_PACKAGE_ROOT}/third_party/ymfm/src/ymfm_adpcm.cpp"
    "${ACESMS_PACKAGE_ROOT}/third_party/ymfm/src/ymfm_pcm.cpp")
target_include_directories(ymfm_example PUBLIC "${ACESMS_PACKAGE_ROOT}/third_party/ymfm/src")
add_library(acesms_fm_ymfm SHARED YmfmPlugin.cpp)
target_include_directories(acesms_fm_ymfm PRIVATE "${ACESMS_PACKAGE_ROOT}/sdk/fm")
target_compile_definitions(acesms_fm_ymfm PRIVATE ACESMS_FM_BUILD_PLUGIN)
target_link_libraries(acesms_fm_ymfm PRIVATE ymfm_example)
set_target_properties(acesms_fm_ymfm PROPERTIES OUTPUT_NAME "acesms-fm-ymfm" PREFIX "")
add_custom_command(TARGET acesms_fm_ymfm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ACESMS_PACKAGE_ROOT}/third_party/ymfm/LICENSE" "$<TARGET_FILE_DIR:acesms_fm_ymfm>/ymfm-LICENSE.txt"
    VERBATIM)
