# vcpkg port for llbt. SHA512 must be filled with the real release-tarball
# hash before submitting this port to a registry (run vcpkg once to get it).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mjm918/llbt
    REF v${VERSION}
    SHA512 0                    # TODO: real hash of the v${VERSION} tarball
    HEAD_REF main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        encryption LLBT_ENABLE_ENCRYPTION
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DLLBT_BUILD_TESTS=OFF
        -DLLBT_BUILD_EXAMPLES=OFF
        ${FEATURE_OPTIONS}
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME llbt CONFIG_PATH lib/cmake/llbt)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/NOTICE"
    "${SOURCE_PATH}/THIRD-PARTY-NOTICES")
