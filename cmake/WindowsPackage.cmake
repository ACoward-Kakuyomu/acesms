install(PROGRAMS "$<TARGET_FILE:smsgg_win32>" DESTINATION . RENAME acesms.exe)
install(FILES "$<TARGET_FILE:acesms_fm_ymfm>" DESTINATION plugins/fm)
install(FILES third_party/ymfm/LICENSE DESTINATION plugins/fm RENAME ymfm-LICENSE.txt)
install(FILES LICENSE DESTINATION .)
install(FILES docs/PACKAGE.md DESTINATION . RENAME README.md)
install(FILES third_party/emu2413/LICENSE DESTINATION third_party/emu2413)
install(FILES third_party/wtl/MS-PL.txt DESTINATION third_party/wtl)
install(FILES third_party/sqlite/README.md third_party/sqlite/PUBLIC-DOMAIN.txt DESTINATION third_party/sqlite)
install(FILES third_party/miniz/README.md third_party/miniz/LICENSE DESTINATION third_party/miniz)
install(FILES third_party/picojson/README.md third_party/picojson/LICENSE DESTINATION third_party/picojson)
install(FILES sdk/fm/acesms_fm.h sdk/fm/README.md sdk/fm/LICENSE DESTINATION sdk/fm)
install(FILES src/plugins/ymfm/CMakeLists.txt src/plugins/ymfm/README.md src/plugins/ymfm/YmfmPlugin.cpp DESTINATION src/plugins/ymfm)
install(FILES third_party/ymfm/LICENSE third_party/ymfm/README.md third_party/ymfm/UPSTREAM-README.md third_party/ymfm/UPSTREAM-SHA256SUMS.txt DESTINATION third_party/ymfm)
install(FILES third_party/ymfm/src/ymfm.h third_party/ymfm/src/ymfm_fm.h third_party/ymfm/src/ymfm_fm.ipp
    third_party/ymfm/src/ymfm_opl.h third_party/ymfm/src/ymfm_opl.cpp
    third_party/ymfm/src/ymfm_adpcm.h third_party/ymfm/src/ymfm_adpcm.cpp
    third_party/ymfm/src/ymfm_pcm.h third_party/ymfm/src/ymfm_pcm.cpp DESTINATION third_party/ymfm/src)
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME acesms)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(ACESMS_PACKAGE_STEM "acesms-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_FILE_NAME "${ACESMS_PACKAGE_STEM}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
include(CPack)
add_custom_target(acesms_package
    COMMAND "${CMAKE_CPACK_COMMAND}" -C "$<CONFIG>" -G ZIP --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
        -B "${CMAKE_BINARY_DIR}/packages/$<CONFIG>"
    DEPENDS smsgg_win32 acesms_fm_ymfm
    VERBATIM)
