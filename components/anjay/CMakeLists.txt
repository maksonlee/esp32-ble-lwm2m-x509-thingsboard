# Adapted from AVSystem/Anjay-esp-idf:
# Copyright 2023-2025 AVSystem <avsystem@avsystem.com>
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#     http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Source layout and ESP32 alignment workaround follow that reference port.
# See ../../README.md for pinned sources and license distinctions.
set(ANJAY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.deps/anjay")
set(PORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.deps/esp-port")
if(NOT EXISTS "${ANJAY_ROOT}/src/anjay_init.h")
    message(FATAL_ERROR "Run python3 tools/prepare_anjay.py before configuring")
endif()
file(GLOB_RECURSE ANJAY_SOURCES
    "${ANJAY_ROOT}/src/*.c"
    "${ANJAY_ROOT}/deps/avs_coap/src/*.c"
    "${ANJAY_ROOT}/deps/avs_commons/src/*.c")
# MD5 streams are a separate upstream library used by HTTP Digest. HTTP and
# downloads are disabled here; that legacy backend is outside this application.
list(FILTER ANJAY_SOURCES EXCLUDE REGEX "/stream/md5/")
idf_component_register(
    SRCS ${ANJAY_SOURCES}
    INCLUDE_DIRS "${PORT_ROOT}/config" "${ANJAY_ROOT}/include_public"
        "${ANJAY_ROOT}/deps/avs_coap/include_public"
        "${ANJAY_ROOT}/deps/avs_commons/include_public"
    PRIV_INCLUDE_DIRS "${ANJAY_ROOT}/src" "${ANJAY_ROOT}/deps/avs_coap/src"
        "${ANJAY_ROOT}/deps/avs_commons/src"
    REQUIRES mbedtls lwip pthread)
# The reference port disables alignment assertions: ESP32 malloc aligns to four
# bytes while GCC reports an eight-byte max_align_t. Do not disable app checks.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NDEBUG)
# Anjay targets C99. GCC 15 defaults to C23, which changes empty parameter lists.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu99 -Wno-error=maybe-uninitialized)
