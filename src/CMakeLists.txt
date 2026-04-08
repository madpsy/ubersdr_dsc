add_library(libdsc SHARED
    dsc_rx.cpp
    firfilter.cpp
    dsc.cpp
    mmsi.cpp
    coast_stations.cpp
    channel_manager.cpp
)

add_executable(dsc_rx_from_file dsc_rx_from_file.cpp)
target_link_libraries(dsc_rx_from_file libdsc)

# -----------------------------------------------------------------------
# dsc_rx_from_ubersdr: receive audio from a ka9q_ubersdr server
# Requires: libzstd, libcurl, IXWebSocket (+ OpenSSL for wss://)
# -----------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD   REQUIRED libzstd)
pkg_check_modules(CURL   REQUIRED libcurl)

# IXWebSocket: prefer a system install, fall back to the copy bundled with
# the ubersdr client tree (../ka9q_ubersdr/clients/CW_Skimmer/UberSDRIntf).
find_package(ixwebsocket QUIET)
if(ixwebsocket_FOUND)
    set(IXWS_LIBS ixwebsocket::ixwebsocket)
    set(IXWS_INCLUDE_DIRS "")
else()
    # Locate the bundled IXWebSocket sources relative to this repo.
    # Adjust IXWS_ROOT if your ubersdr checkout is elsewhere.
    set(IXWS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../ka9q_ubersdr/clients/CW_Skimmer/UberSDRIntf/IXWebSocket"
        CACHE PATH "Path to IXWebSocket source tree")

    if(NOT EXISTS "${IXWS_ROOT}/ixwebsocket/IXWebSocket.h")
        message(FATAL_ERROR
            "IXWebSocket not found.\n"
            "Either install it (e.g. apt install libixwebsocket-dev) or set\n"
            "  -DIXWS_ROOT=/path/to/IXWebSocket\n"
            "pointing at a directory that contains ixwebsocket/IXWebSocket.h")
    endif()

    # Collect all .cpp files from the bundled tree
    file(GLOB IXWS_SOURCES "${IXWS_ROOT}/ixwebsocket/*.cpp")

    add_library(ixwebsocket_bundled STATIC ${IXWS_SOURCES})
    set_target_properties(ixwebsocket_bundled PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ixwebsocket_bundled PUBLIC "${IXWS_ROOT}")
    find_package(OpenSSL REQUIRED)
    target_link_libraries(ixwebsocket_bundled PUBLIC OpenSSL::SSL OpenSSL::Crypto)

    set(IXWS_LIBS ixwebsocket_bundled)
    set(IXWS_INCLUDE_DIRS "${IXWS_ROOT}")
endif()

# pthreads (needed for multi-frequency channel management)
find_package(Threads REQUIRED)

# libdsc needs IXWebSocket, zstd, curl for channel_manager.cpp
target_include_directories(libdsc PUBLIC
    ${IXWS_INCLUDE_DIRS}
    ${ZSTD_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)
target_link_libraries(libdsc
    ${IXWS_LIBS}
    ${ZSTD_LIBRARIES}
    ${CURL_LIBRARIES}
    Threads::Threads
    m
)

add_executable(dsc_rx_from_ubersdr dsc_rx_from_ubersdr.cpp)

target_include_directories(dsc_rx_from_ubersdr PRIVATE
    ${IXWS_INCLUDE_DIRS}
    ${ZSTD_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(dsc_rx_from_ubersdr
    libdsc
    ${IXWS_LIBS}
    ${ZSTD_LIBRARIES}
    ${CURL_LIBRARIES}
    Threads::Threads
    m
)

include(GNUInstallDirs)
install(TARGETS libdsc dsc_rx_from_file dsc_rx_from_ubersdr)
