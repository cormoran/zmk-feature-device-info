zephyr_include_directories(include)
if(CONFIG_ZMK_DEVICE_INFO)
    # The nanopb-generated proto, the git-version defines, the GNU build-id and
    # the shared device_info_collect are needed by BOTH the central Studio RPC
    # handler and the split peripheral relay path (which collects and encodes
    # the peripheral's own DeviceInfoResponse). The peripheral image has no
    # ZMK_STUDIO, so this must NOT be gated on it.
    if(CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC OR CONFIG_ZMK_DEVICE_INFO_SPLIT)
        # Inject git version of ZMK application source
        execute_process(
            COMMAND git -C ${APPLICATION_SOURCE_DIR} describe --dirty --always --tags
            OUTPUT_VARIABLE ZMK_BUILD_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT ZMK_BUILD_VERSION)
            set(ZMK_BUILD_VERSION "unknown")
        endif()

        # Inject git version of zmk-config directory (if it is a git repo)
        set(ZMK_CONFIG_BUILD_VERSION "")
        if(DEFINED ZMK_CONFIG AND EXISTS "${ZMK_CONFIG}")
            execute_process(
                COMMAND git -C ${ZMK_CONFIG} rev-parse --git-dir
                RESULT_VARIABLE _zmk_config_git_result
                OUTPUT_QUIET ERROR_QUIET
            )
            if(_zmk_config_git_result EQUAL 0)
                execute_process(
                    COMMAND git -C ${ZMK_CONFIG} describe --dirty --always --tags
                    OUTPUT_VARIABLE ZMK_CONFIG_BUILD_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                )
            endif()
        endif()
        if(NOT ZMK_CONFIG_BUILD_VERSION)
            set(ZMK_CONFIG_BUILD_VERSION "")
        endif()

        # Inject git version of this module
        execute_process(
            COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} describe --dirty --always --tags
            OUTPUT_VARIABLE MODULE_BUILD_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT MODULE_BUILD_VERSION)
            set(MODULE_BUILD_VERSION "unknown")
        endif()

        # Inject build timestamp (UTC)
        string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

        target_compile_definitions(app PRIVATE
            ZMK_BUILD_VERSION="${ZMK_BUILD_VERSION}"
            ZMK_CONFIG_BUILD_VERSION="${ZMK_CONFIG_BUILD_VERSION}"
            MODULE_BUILD_VERSION="${MODULE_BUILD_VERSION}"
            BUILD_TIMESTAMP="${BUILD_TIMESTAMP}"
        )

        # Emit a GNU build-id (SHA1 of the linked image) so a running device can
        # be matched back to the exact ELF for crash/error analysis. Zephyr sets
        # `--build-id=none` in its linker `base` property; modules are processed
        # after that on the final link line, so this later flag wins (ld honors
        # the last --build-id). build_id.ld pins the note into flash so it ships
        # in the image and can be read at runtime.
        #
        # Skipped on native_sim/posix: it is a host executable (no flash image to
        # match, and no ELF error analysis) and its two-stage native link discards
        # the .note.gnu.build-id section, which would dangle the boundary symbols.
        if(NOT CONFIG_ARCH_POSIX)
            zephyr_link_libraries(-Wl,--build-id=sha1)
            zephyr_linker_sources(ROM_SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/src/build_id.ld)
        endif()

        target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device_info_collect.c)

        list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
        include(nanopb)
        set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
        set(NANOPB_GENERATE_CPP_STANDALONE OFF)

        # NOTE: adding to app target directly causes build issues, so we create a library instead
        zephyr_library()
        file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

        nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
        target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
        target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

        # app should depend on the generated proto files
        target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
        add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
    endif()

    if(CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC)
        file(GLOB_RECURSE STUDIO_C_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/*.c)
        target_sources(app PRIVATE ${STUDIO_C_FILES})
    endif()

    # Split event-relay glue: relay a device-info collect request to the
    # peripheral half/halves and deliver their info back to the PC as RPC
    # notifications. Compiled on both split roles; the relay macros are
    # self-role-gating.
    if(CONFIG_ZMK_DEVICE_INFO_SPLIT)
        file(GLOB_RECURSE SPLIT_C_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/split/*.c)
        target_sources(app PRIVATE ${SPLIT_C_FILES})
    endif()
endif()
