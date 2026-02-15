cmake_minimum_required(VERSION 3.16)
project(sip_event_processor VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Dependency root — all custom libraries live under this prefix
#   /opt/sip_event_processor/SofiaSip/
#   /opt/sip_event_processor/mongo-c-driver/
#   /opt/sip_event_processor/mongodbpool/
# Override with: cmake -DDEPS_ROOT=/other/path ..
# ---------------------------------------------------------------------------
set(DEPS_ROOT "/opt/sip_event_processor" CACHE PATH "Root directory for all custom dependencies")

set(SOFIA_SIP_ROOT   "${DEPS_ROOT}/SofiaSip"       CACHE PATH "Sofia-SIP install prefix")
set(MONGO_C_ROOT     "${DEPS_ROOT}/mongo-c-driver"  CACHE PATH "MongoDB C driver install prefix")
set(MONGODBPOOL_ROOT "${DEPS_ROOT}/mongodbpool"     CACHE PATH "Custom MongoPool library install prefix")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

# ---------------------------------------------------------------------------
# Sofia-SIP (pkg-config from custom prefix)
# ---------------------------------------------------------------------------
set(ENV{PKG_CONFIG_PATH}
    "${SOFIA_SIP_ROOT}/lib/pkgconfig:${SOFIA_SIP_ROOT}/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SOFIA REQUIRED sofia-sip-ua)

# ---------------------------------------------------------------------------
# MongoDB C driver — libmongoc + libbson (pkg-config from custom prefix)
# ---------------------------------------------------------------------------
set(ENV{PKG_CONFIG_PATH}
    "${MONGO_C_ROOT}/lib/pkgconfig:${MONGO_C_ROOT}/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")

pkg_check_modules(MONGOC REQUIRED libmongoc-1.0)
pkg_check_modules(BSON   REQUIRED libbson-1.0)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
add_executable(sip_event_processor
    src/main.cpp
    src/common/logger.cpp
    src/common/config.cpp
    src/common/slow_event_logger.cpp
    src/sip/sip_event.cpp
    src/sip/sip_dialog_id.cpp
    src/sip/sip_callback_handler.cpp
    src/sip/sip_stack_manager.cpp
    src/dispatch/dialog_worker.cpp
    src/dispatch/dialog_dispatcher.cpp
    src/dispatch/stale_subscription_reaper.cpp
    src/subscription/subscription_state.cpp
    src/subscription/blf_subscription_index.cpp
    src/subscription/blf_processor.cpp
    src/subscription/mwi_processor.cpp
    src/presence/presence_xml_parser.cpp
    src/presence/presence_tcp_client.cpp
    src/presence/presence_event_router.cpp
    src/presence/presence_failover_manager.cpp
    src/persistence/mongo_client.cpp
    src/persistence/subscription_store.cpp
    src/http/http_server.cpp
    src/http/health_handler.cpp
    src/http/stats_handler.cpp
)

# ---------------------------------------------------------------------------
# RPATH — so the binary finds .so files at runtime from custom paths
# ---------------------------------------------------------------------------
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)

set(_EXTRA_RPATH
    "${SOFIA_SIP_ROOT}/lib"
    "${MONGO_C_ROOT}/lib"
    "${MONGO_C_ROOT}/lib64"
    "${MONGODBPOOL_ROOT}/lib"
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(sip_event_processor PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${MONGODBPOOL_ROOT}/include
    ${SOFIA_INCLUDE_DIRS}
    ${MONGOC_INCLUDE_DIRS}
    ${BSON_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------------
# Library search directories
# ---------------------------------------------------------------------------
target_link_directories(sip_event_processor PRIVATE
    ${SOFIA_LIBRARY_DIRS}
    ${MONGOC_LIBRARY_DIRS}
    ${BSON_LIBRARY_DIRS}
    ${MONGO_C_ROOT}/lib
    ${MONGO_C_ROOT}/lib64
    ${MONGODBPOOL_ROOT}/lib
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(sip_event_processor PRIVATE
    ${SOFIA_LIBRARIES}
    ${MONGOC_LIBRARIES}
    ${BSON_LIBRARIES}
    mongodbpool
    pthread
)

set_target_properties(sip_event_processor PROPERTIES
    BUILD_RPATH   "${_EXTRA_RPATH}"
    INSTALL_RPATH "${_EXTRA_RPATH}"
)
