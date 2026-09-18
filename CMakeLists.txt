cmake_minimum_required(VERSION 3.5...3.29)

# Set extension name here
set(TARGET_NAME glue)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The AWS SDK (installed through vcpkg) provides the Glue Data Catalog client
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS core glue sts sso)

# The AWS find_package can override TARGET_NAME, reset it
set(TARGET_NAME glue)

include_directories(src/include)
include_directories(${CURL_INCLUDE_DIRS})
include_directories(${AWSSDK_INCLUDE_DIRS})

set(EXTENSION_SOURCES
    src/glue_extension.cpp
    src/glue_attach.cpp
    src/glue_api.cpp
    src/glue_types.cpp
    src/glue_http_client.cpp
    src/glue_functions.cpp
    src/glue_partition_functions.cpp
    src/glue_grammar.cpp
    src/hive_scan_function.cpp
    src/storage/glue_catalog.cpp
    src/storage/glue_schema_set.cpp
    src/storage/glue_schema_entry.cpp
    src/storage/glue_table_set.cpp
    src/storage/glue_table.cpp
    src/storage/hive_multi_file_reader.cpp
    src/storage/glue_hive_insert.cpp
    src/storage/glue_metadata_cache.cpp
    src/storage/glue_transaction.cpp
    src/storage/glue_transaction_manager.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link the AWS SDK, CURL and OpenSSL in both the static library and the loadable
# extension
target_include_directories(${EXTENSION_NAME}
                           PUBLIC $<BUILD_INTERFACE:${AWSSDK_INCLUDE_DIRS}>)
target_link_libraries(${EXTENSION_NAME} ${AWSSDK_LINK_LIBRARIES}
                      ${CURL_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(${LOADABLE_EXTENSION_NAME}
                           PRIVATE $<BUILD_INTERFACE:${AWSSDK_INCLUDE_DIRS}>)
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${AWSSDK_LINK_LIBRARIES}
                      ${CURL_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
