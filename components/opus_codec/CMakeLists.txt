# Opus Codec Component for ESP32-S3
#
# This component wraps libopus to provide encoder/decoder functionality.
# Libopus source is downloaded and built automatically via CMake FetchContent.
#

set(OPUS_GIT_REPOSITORY "https://github.com/xiph/opus.git")
set(OPUS_GIT_TAG "v1.5.2")

# Include this component
idf_component_register(
    SRCS 
        "opus_codec.c"
    INCLUDE_DIRS 
        "."
    REQUIRES 
        esp_system
)

# Fetch and build libopus from source
include(FetchContent)

# Disable tests and programs - we only need the library
set(OPUS_BUILD_SHARED_LIBS OFF)
set(OPUS_BUILD_PROGRAMS OFF)
set(OPUS_BUILD_TESTING OFF)
set(BUILD_TESTING OFF)
set(OPUS_INSTALL_LIBDIR "lib")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Configure for fixed-point (faster on ESP32)
set(OPUS_FIXED_POINT ON)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)

FetchContent_Declare(
    opus_src
    GIT_REPOSITORY ${OPUS_GIT_REPOSITORY}
    GIT_TAG ${OPUS_GIT_TAG}
    SOURCE_SUBDIR "."
)

FetchContent_GetProperties(opus_src)
if(NOT opus_src_POPULATED)
    message(STATUS "Fetching opus source from ${OPUS_GIT_REPOSITORY} (tag ${OPUS_GIT_TAG})")
    FetchContent_Populate(opus_src)
endif()

# Disable install/export for opus (not needed in ESP-IDF)
set(CMAKE_SKIP_INSTALL_ALL_TARGETS TRUE)
set(CMAKE_SKIP_INSTALL_EXPORTS TRUE)

# Build the opus library
add_subdirectory(${opus_src_SOURCE_DIR} ${opus_src_BINARY_DIR} EXCLUDE_FROM_ALL)

# Link the opus library to this component
target_link_libraries(${COMPONENT_LIB} PUBLIC opus)

# Add opus include directories
target_include_directories(${COMPONENT_LIB} PUBLIC 
    ${opus_src_SOURCE_DIR}/include
    ${opus_src_SOURCE_DIR}/src
)
