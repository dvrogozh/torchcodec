# FindTorchCodec
# --------------
#
# Finds the TorchCodec library
#
# This will define the following variables:
#
#   TORCHCODEC_FOUND        -- True if the system has the TorchCodec library
#   TORCHCODEC_VARIANTS     -- List of TorchCodec variants
#
# and the following imported targets:
#
#   torchcodec::ffmpeg${N}
#   torchcodec::core${N}
#
# where N is a TorchCodec variant from TORCHCODEC_VARIANTS list.

include(FindPackageHandleStandardArgs)
include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg_versions.cmake")

# Assume we are in <install-prefix>/share/cmake/TorchCodec/TorchCodecConfig.cmake
get_filename_component(CMAKE_CURRENT_LIST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
get_filename_component(TORCHCODEC_INSTALL_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

# Include directories.
set(TORCHCODEC_INCLUDE_DIRS ${TORCHCODEC_INSTALL_PREFIX}/_core)
set(TORCHCODEC_VARIANTS "")

function(add_torchcodec_target ffmpeg_major_version)
    set(target torchcodec::core${ffmpeg_major_version})

    if (NOT TARGET torchcodec::ffmpeg${ffmpeg_major_version})
        message("Skipping ${target} as torchcodec::ffmpeg${ffmpeg_major_version} is not defined")
        return()
    endif()

    find_library(_LIB_PATH torchcodec_core${ffmpeg_major_version}
        PATHS "${TORCHCODEC_INSTALL_PREFIX}" NO_CACHE NO_DEFAULT_PATH)
    if (NOT _LIB_PATH)
        message("Skipping ${target} as torchcodec_core${ffmpeg_major_version} is missing")
	return()
    endif()

    message("Adding ${target} target")
    add_library(${target} SHARED IMPORTED)
    add_dependencies(${target} torchcodec::ffmpeg${ffmpeg_major_version})
    set_target_properties(${target} PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES ${TORCHCODEC_INCLUDE_DIRS}
      IMPORTED_LOCATION ${_LIB_PATH}
    )
    # Removing _LIB_PATH from cache otherwise it won't be updated
    # on the next call to find_library().
    unset(_LIB_PATH CACHE)

    list(APPEND TORCHCODEC_VARIANTS "${ffmpeg_major_version}")
    set(TORCHCODEC_VARIANTS "${TORCHCODEC_VARIANTS}" PARENT_SCOPE)
endfunction()

set(USE_PKG_CONFIG TRUE)
foreach(ffmpeg_major_version IN LISTS TORCHCODEC_SUPPORTED_FFMPEG_VERSIONS)
    if ((DEFINED ENV{TORCHCODEC_FFMPEG${ffmpeg_major_version}_INSTALL_PREFIX})
        add_ffmpeg_target(
            "${ffmpeg_major_version}"
            "$ENV{TORCHCODEC_FFMPEG${ffmpeg_major_version}_INSTALL_PREFIX}"
        )
        add_torchcodec_target(${ffmpeg_major_version})
	set(USE_PKG_CONFIG FALSE)
    endif
endforeach()

if (USE_PKG_CONFIG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TORCHCODEC_LIBAV IMPORTED_TARGET
        libavdevice
        libavfilter
        libavformat
        libavcodec
        libavutil
        libswresample
        libswscale
    )

    if (TARGET PkgConfig::TORCHCODEC_LIBAV)
        # Split libavcodec's version string by '.' and convert it to a list
        string(REPLACE "." ";" libavcodec_version_list ${TORCHCODEC_LIBAV_libavcodec_VERSION})
        # Get the first element of the list, which is the major version
        list(GET libavcodec_version_list 0 libavcodec_major_version)

        if (${libavcodec_major_version} STREQUAL "58")
            set(ffmpeg_major_version "4")
        elseif (${libavcodec_major_version} STREQUAL "59")
            set(ffmpeg_major_version "5")
        elseif (${libavcodec_major_version} STREQUAL "60")
            set(ffmpeg_major_version "6")
        elseif (${libavcodec_major_version} STREQUAL "61")
            set(ffmpeg_major_version "7")
        endif()

        if (libavcodec_major_version)
            message("Adding torchcodec::ffmpeg${ffmpeg_major_version} target")
            add_library(torchcodec::ffmpeg${ffmpeg_major_version} ALIAS PkgConfig::TORCHCODEC_LIBAV)
            add_torchcodec_target(${ffmpeg_major_version})
        endif()
    endif()
endif()

find_package_handle_standard_args(TorchCodec DEFAULT_MSG TORCHCODEC_VARIANTS)
