cmake_minimum_required(VERSION 3.16)
project(sip_event_processor VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SOFIA REQUIRED sofia-sip-ua)
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)

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

target_include_directories(sip_event_processor PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${SOFIA_INCLUDE_DIRS}
)

target_link_libraries(sip_event_processor PRIVATE
    ${SOFIA_LIBRARIES}
    mongo::mongocxx_shared
    mongo::bsoncxx_shared
    pthread
)
