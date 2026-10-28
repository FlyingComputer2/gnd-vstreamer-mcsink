# gnd-vstreamer-mcsink: ground-station receiver for the ground P4's RTP H.264
# multicast. multicast_source -> rtp_h264_depay -> h264_decoder_mpp -> OSD
# (bitrate, camera-to-decode latency, decode fps) -> sdl_sink (KMSDRM or a
# window). The vstreamer components come from a pinned vstreamer.
# Local checkout: -DFETCHCONTENT_SOURCE_DIR_VSTREAMER=<dir>.
cmake_minimum_required(VERSION 3.16)
project(gnd_vstreamer_mcsink CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Only the components this receiver uses.
foreach(opt ENABLE_NOISE_SOURCE ENABLE_V4L2_SOURCE ENABLE_JPEG_DECODER_MULTICORE
            ENABLE_MKV_SINK ENABLE_STREAM_SENDER ENABLE_STREAM_RECEIVER
            ENABLE_RTP_H264_PAY ENABLE_APP_UVC_STREAM_SENDER
            ENABLE_APP_SDL_STREAM_RECEIVER ENABLE_APP_VSTREAMER ENABLE_APP_CHANNEL_SIM
            VSTREAMER_BUILD_TESTS)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt ENABLE_H264_DECODER_MPP ENABLE_SDL_SINK ENABLE_RTP_H264_DEPAY)
    set(${opt} ON CACHE BOOL "" FORCE)
endforeach()

include(FetchContent)
FetchContent_Declare(vstreamer
    GIT_REPOSITORY https://github.com/FlyingComputer2/vstreamer.git
    GIT_TAG 718562677e795b44c3ff5c96633af80f35529b9d
)
FetchContent_MakeAvailable(vstreamer)

add_executable(gnd-vstreamer-mcsink
    src/main.cpp
    src/multicast_source.cpp
    src/osd_overlay.cpp
    src/osd_font.cpp
)
target_include_directories(gnd-vstreamer-mcsink PRIVATE src)
target_link_libraries(gnd-vstreamer-mcsink PRIVATE vstreamer_source)
target_compile_options(gnd-vstreamer-mcsink PRIVATE -Wall -Wextra)
install(TARGETS gnd-vstreamer-mcsink RUNTIME DESTINATION bin)

option(MCSINK_BUILD_TESTS "Build host tests" OFF)
if(MCSINK_BUILD_TESTS)
    enable_testing()
    add_executable(mcsink_tests tests/mcsink_tests.cpp src/osd_overlay.cpp src/osd_font.cpp)
    target_include_directories(mcsink_tests PRIVATE src)
    target_link_libraries(mcsink_tests PRIVATE vstreamer_source)
    add_test(NAME mcsink_tests COMMAND mcsink_tests)
endif()
