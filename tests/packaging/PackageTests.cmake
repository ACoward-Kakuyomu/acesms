execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" --config "${CONFIG}" --target acesms_package
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Package target failed (${result}):\n${output}\n${errors}")
endif()
set(archive "${BINARY_DIR}/packages/${CONFIG}/${PACKAGE_STEM}.zip")
file(SHA256 "${archive}" hash)
set(extract "${BINARY_DIR}/package-check/${CONFIG}/${hash}")
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${extract}")
file(GLOB top RELATIVE "${extract}" "${extract}/*")
if(NOT top STREQUAL PACKAGE_STEM)
    message(FATAL_ERROR "Unexpected archive top-level entries: ${top}")
endif()
set(root "${extract}/${PACKAGE_STEM}")
set(pairs
    "acesms.exe|${APP}" "plugins/fm/acesms-fm-ymfm.dll|${PLUGIN}"
    "README.md|${SOURCE_DIR}/docs/PACKAGE.md" "LICENSE|${SOURCE_DIR}/LICENSE"
    "third_party/emu2413/LICENSE|${SOURCE_DIR}/third_party/emu2413/LICENSE"
    "third_party/wtl/MS-PL.txt|${SOURCE_DIR}/third_party/wtl/MS-PL.txt"
    "third_party/sqlite/README.md|${SOURCE_DIR}/third_party/sqlite/README.md"
    "third_party/sqlite/PUBLIC-DOMAIN.txt|${SOURCE_DIR}/third_party/sqlite/PUBLIC-DOMAIN.txt"
    "third_party/miniz/README.md|${SOURCE_DIR}/third_party/miniz/README.md"
    "third_party/miniz/LICENSE|${SOURCE_DIR}/third_party/miniz/LICENSE"
    "third_party/picojson/README.md|${SOURCE_DIR}/third_party/picojson/README.md"
    "third_party/picojson/LICENSE|${SOURCE_DIR}/third_party/picojson/LICENSE"
    "plugins/fm/ymfm-LICENSE.txt|${SOURCE_DIR}/third_party/ymfm/LICENSE")
foreach(file IN ITEMS sdk/fm/acesms_fm.h sdk/fm/README.md sdk/fm/LICENSE
    src/plugins/ymfm/CMakeLists.txt src/plugins/ymfm/README.md src/plugins/ymfm/YmfmPlugin.cpp
    third_party/ymfm/LICENSE third_party/ymfm/README.md third_party/ymfm/UPSTREAM-README.md third_party/ymfm/UPSTREAM-SHA256SUMS.txt
    third_party/ymfm/src/ymfm.h third_party/ymfm/src/ymfm_fm.h third_party/ymfm/src/ymfm_fm.ipp
    third_party/ymfm/src/ymfm_opl.h third_party/ymfm/src/ymfm_opl.cpp third_party/ymfm/src/ymfm_adpcm.h third_party/ymfm/src/ymfm_adpcm.cpp
    third_party/ymfm/src/ymfm_pcm.h third_party/ymfm/src/ymfm_pcm.cpp)
    list(APPEND pairs "${file}|${SOURCE_DIR}/${file}")
endforeach()
set(expected)
foreach(pair IN LISTS pairs)
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 destination)
    list(GET fields 1 source)
    file(SHA256 "${source}" sourceHash)
    file(SHA256 "${root}/${destination}" actualHash)
    if(NOT sourceHash STREQUAL actualHash)
        message(FATAL_ERROR "Changed or missing package file: ${destination}")
    endif()
    list(APPEND expected "${destination}")
endforeach()
file(GLOB_RECURSE actual LIST_DIRECTORIES false RELATIVE "${root}" "${root}/*")
list(SORT actual)
list(SORT expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Package file whitelist differs:\nactual=${actual}\nexpected=${expected}")
endif()
if(NOT POINTER_BYTES EQUAL 8)
    message(FATAL_ERROR "Package architecture differs")
endif()
file(SHA256 "${root}/LICENSE" mit)
file(SHA256 "${root}/sdk/fm/LICENSE" sdkMit)
if(NOT mit STREQUAL sdkMit)
    message(FATAL_ERROR "SDK MIT differs from project MIT")
endif()
execute_process(COMMAND powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${SOURCE_DIR}/tests/packaging/CheckWindowsResources.ps1" -Exe "${root}/acesms.exe" -Version "${VERSION}"
    RESULT_VARIABLE resourceResult)
if(NOT resourceResult EQUAL 0)
    message(FATAL_ERROR "Packaged Windows resources failed")
endif()
message(STATUS "Package whitelist/bytes/licenses/configuration PASS: ${root}")
file(WRITE "${BINARY_DIR}/package-check/${CONFIG}/latest.txt" "${root}")
