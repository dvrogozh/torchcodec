set(TORCHCODEC_SUPPORTED_FFMPEG_VERSIONS "4;5;6;7;8")

if (UNIX AND NOT APPLE)
    set(
       f4_library_file_names
       libavutil.so.56
       libavcodec.so.58
       libavformat.so.58
       libavdevice.so.58
       libavfilter.so.7
       libswscale.so.5
       libswresample.so.3
    )
    set(
       f5_library_file_names
       libavutil.so.57
       libavcodec.so.59
       libavformat.so.59
       libavdevice.so.59
       libavfilter.so.8
       libswscale.so.6
       libswresample.so.4
    )
    set(
       f6_library_file_names
       libavutil.so.58
       libavcodec.so.60
       libavformat.so.60
       libavdevice.so.60
       libavfilter.so.9
       libswscale.so.7
       libswresample.so.4
    )
    set(
       f7_library_file_names
       libavutil.so.59
       libavcodec.so.61
       libavformat.so.61
       libavdevice.so.61
       libavfilter.so.10
       libswscale.so.8
       libswresample.so.5
    )
    set(
       f8_library_file_names
       libavutil.so.60
       libavcodec.so.62
       libavformat.so.62
       libavdevice.so.62
       libavfilter.so.11
       libswscale.so.9
       libswresample.so.6
    )
elseif (APPLE)
    set(
       f4_library_file_names
       libavutil.56.dylib
       libavcodec.58.dylib
       libavformat.58.dylib
       libavdevice.58.dylib
       libavfilter.7.dylib
       libswscale.5.dylib
       libswresample.3.dylib
    )
    set(
       f5_library_file_names
       libavutil.57.dylib
       libavcodec.59.dylib
       libavformat.59.dylib
       libavdevice.59.dylib
       libavfilter.8.dylib
       libswscale.6.dylib
       libswresample.4.dylib
    )
    set(
       f6_library_file_names
       libavutil.58.dylib
       libavcodec.60.dylib
       libavformat.60.dylib
       libavdevice.60.dylib
       libavfilter.9.dylib
       libswscale.7.dylib
       libswresample.4.dylib
    )
    set(
       f7_library_file_names
       libavutil.59.dylib
       libavcodec.61.dylib
       libavformat.61.dylib
       libavdevice.61.dylib
       libavfilter.10.dylib
       libswscale.8.dylib
       libswresample.5.dylib
    )
    set(
       f8_library_file_names
       libavutil.60.dylib
       libavcodec.62.dylib
       libavformat.62.dylib
       libavdevice.62.dylib
       libavfilter.11.dylib
       libswscale.9.dylib
       libswresample.6.dylib
    )
elseif (WIN32)
    set(
        f4_library_file_names
        avutil.lib
        avcodec.lib
        avformat.lib
        avdevice.lib
        avfilter.lib
        swscale.lib
        swresample.lib
    )
    set(
        f5_library_file_names
        avutil.lib
        avcodec.lib
        avformat.lib
        avdevice.lib
        avfilter.lib
        swscale.lib
        swresample.lib
    )
    set(
        f6_library_file_names
        avutil.lib
        avcodec.lib
        avformat.lib
        avdevice.lib
        avfilter.lib
        swscale.lib
        swresample.lib
    )
    set(
        f7_library_file_names
        avutil.lib
        avcodec.lib
        avformat.lib
        avdevice.lib
        avfilter.lib
        swscale.lib
        swresample.lib
    )
    set(
        f8_library_file_names
        avutil.lib
        avcodec.lib
        avformat.lib
        avdevice.lib
        avfilter.lib
        swscale.lib
        swresample.lib
    )
else()
    message(
        FATAL_ERROR
        "Unsupported operating system: ${CMAKE_SYSTEM_NAME}"
    )
endif()

function(add_ffmpeg_target ffmpeg_major_version prefix)
    list(FIND TORCHCODEC_SUPPORTED_FFMPEG_VERSIONS "${ffmpeg_major_version}" _index)
    if (_index LESS 0)
        message(FATAL_ERROR "FFmpeg version ${ffmpeg_major_version} is not supported")
    endif()
    if (NOT DEFINED prefix)
        message(FATAL_ERROR "No prefix defined calling add_ffmpeg_target()")
    endif()

    set(target "torchcodec::ffmpeg${ffmpeg_major_version}")
    set(incdir "${prefix}/include")
    if (UNIX OR APPLE)
        set(libdir "${prefix}/lib")
    elseif (WIN32)
        set(libdir "${prefix}/bin")
    else()
        message(FATAL_ERROR "Unsupported operating system: ${CMAKE_SYSTEM_NAME}")
    endif()

    list(
        TRANSFORM f${ffmpeg_major_version}_library_file_names
        PREPEND ${libdir}/
        OUTPUT_VARIABLE lib_paths
    )

    message("Adding ${target} target")
    # Verify that ffmpeg includes and libraries actually exist.
    foreach (path IN LISTS incdir lib_paths)
        if (NOT EXISTS "${path}")
            message(FATAL_ERROR "${path} does not exist")
        endif()
    endforeach()

    add_library(${target} INTERFACE IMPORTED)
    target_include_directories(${target} INTERFACE ${incdir})
    target_link_libraries(${target} INTERFACE ${lib_paths})
endfunction()
