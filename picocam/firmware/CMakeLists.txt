cmake_minimum_required(VERSION 3.10)
# Cross-compile for Luckfox Pico / Pico Plus (uClibc). Use ../build.sh (Docker image luckfox-ai).
project(picocam C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections")

set(RKMPI_DIR "/opt/rkmpi" CACHE PATH "luckfox_pico_rkmpi_example checkout (headers + uClibc libs)")
set(RKNPU2_DIR "/opt/rknpu2" CACHE PATH "rknn_api.h + armhf-uclibc/librknnmrt.so")
set(PICOCAM_VERSION "dev" CACHE STRING "version string")
set(RKMPI_LIB ${RKMPI_DIR}/lib/uclibc)

set(OpenCV_DIR "${RKMPI_LIB}/lib/cmake/opencv4")
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# mbedTLS (HTTPS server, https:// webhooks, MQTT over TLS): the library sources in third_party/ built as a static lib
# (its own CMake wants a git submodule that is not needed just to compile the library)
file(GLOB MBEDTLS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/library/*.c)
add_library(mbedtls_all STATIC ${MBEDTLS_SRC})
target_include_directories(mbedtls_all PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/library)
target_compile_options(mbedtls_all PRIVATE -O2 -ffunction-sections -fdata-sections -w)

file(GLOB_RECURSE SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)
add_executable(picocam ${SRC})

target_include_directories(picocam PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
  ${RKNPU2_DIR}/include
  ${RKMPI_DIR}/include
  ${RKMPI_DIR}/include/librga
  ${RKMPI_DIR}/include/rkaiq
  ${RKMPI_DIR}/include/rkaiq/uAPI2
  ${RKMPI_DIR}/include/rkaiq/common
  ${RKMPI_DIR}/include/rkaiq/xcore
  ${RKMPI_DIR}/include/rkaiq/algos
  ${RKMPI_DIR}/include/rkaiq/iq_parser
  ${RKMPI_DIR}/include/rkaiq/iq_parser_v2
  ${OpenCV_INCLUDE_DIRS})

target_compile_definitions(picocam PRIVATE RV1106_1103 RKPLATFORM=ON ARCH64=OFF UAPI2
  PICOCAM_VERSION="${PICOCAM_VERSION}"
  _LARGEFILE_SOURCE _LARGEFILE64_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(picocam PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers)

target_link_directories(picocam PRIVATE ${RKMPI_LIB})
target_link_libraries(picocam
  ${OpenCV_LIBS}
  ${RKNPU2_DIR}/armhf-uclibc/librknnmrt.so
  rockit rockchip_mpp rkaiq rga
  mbedtls_all dl
  Threads::Threads)
set_target_properties(picocam PROPERTIES LINK_FLAGS "-Wl,-rpath-link,${RKMPI_LIB}")
