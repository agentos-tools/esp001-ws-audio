# Opus Codec Component for ESP32-S3
#
# This component wraps libopus to provide encoder/decoder functionality.
# Uses ExternalProject to build opus as part of the ESP-IDF build.
#

set(OPUS_VERSION "1.5.2")
# The opus archive is in the sibling opus component directory
set(OPUS_ARCHIVE "${CMAKE_CURRENT_LIST_DIR}/../opus/opus-${OPUS_VERSION}.tar.gz")
set(OPUS_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/opus-${OPUS_VERSION}")
set(OPUS_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/opus_build")

# Extract opus source during configure phase
if(NOT EXISTS "${OPUS_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "Extracting opus source from: ${OPUS_ARCHIVE}")
    if(NOT EXISTS "${OPUS_ARCHIVE}")
        message(FATAL_ERROR "Opus archive not found: ${OPUS_ARCHIVE}")
    endif()
    file(REMOVE_RECURSE "${OPUS_SOURCE_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${OPUS_ARCHIVE}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        RESULT_VARIABLE rv
    )
    if(NOT rv EQUAL 0)
        message(FATAL_ERROR "Failed to extract opus archive")
    endif()
endif()

# Include this component
idf_component_register(
    SRCS 
        "opus_codec.c"
    INCLUDE_DIRS 
        "."
        "${OPUS_SOURCE_DIR}/include"
    REQUIRES 
        esp_system
)

# Configure and build opus using ExternalProject
include(ExternalProject)

ExternalProject_Add(
    opus_ep
    SOURCE_DIR "${OPUS_SOURCE_DIR}"
    BINARY_DIR "${OPUS_BUILD_DIR}"
    INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/opus_install"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DOPUS_BUILD_SHARED_LIBS=OFF
        -DOPUS_BUILD_PROGRAMS=OFF
        -DOPUS_BUILD_TESTING=OFF
        -DBUILD_TESTING=OFF
        -DOPUS_FIXED_POINT=ON
        -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
        -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF
        -DCMAKE_SKIP_INSTALL_ALL_TARGETS=ON
        -DCMAKE_SKIP_INSTALL_EXPORTS=ON
    BUILD_BYPRODUCTS "${OPUS_BUILD_DIR}/libopus.a"
    EXCLUDE_FROM_MAIN 1
)

# Create imported target for the static library
add_library(opus_lib STATIC IMPORTED GLOBAL)
set_target_properties(opus_lib PROPERTIES
    IMPORTED_LOCATION "${OPUS_BUILD_DIR}/libopus.a"
)
add_dependencies(opus_lib opus_ep)

# Link opus to this component
target_link_libraries(${COMPONENT_LIB} PUBLIC opus_lib)

# Add opus include directories
target_include_directories(${COMPONENT_LIB} PUBLIC 
    "${OPUS_SOURCE_DIR}/include"
)
