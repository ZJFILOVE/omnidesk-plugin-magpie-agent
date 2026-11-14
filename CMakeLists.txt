cmake_minimum_required(VERSION 3.16)
project(omnidesk_plugin_magpie_agent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(OmniDeskSDK REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(plugin_magpie_agent_core STATIC
    src/process_exec.cpp
    src/http_client.cpp
    src/magpie_parser.cpp
    src/magpie_client.cpp
    src/magpie_ui.cpp
)
set_target_properties(plugin_magpie_agent_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(plugin_magpie_agent_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(plugin_magpie_agent_core
    PUBLIC
        OmniDesk::SDK
        CURL::libcurl
        Threads::Threads
)

add_library(plugin_magpie_agent SHARED
    src/plugin_main.cpp
)
target_link_libraries(plugin_magpie_agent
    PRIVATE
        plugin_magpie_agent_core
)
set_target_properties(plugin_magpie_agent PROPERTIES
    OUTPUT_NAME "plugin_magpie_agent"
)

enable_testing()

add_executable(test_abi tests/test_abi.cpp)
target_link_libraries(test_abi PRIVATE OmniDesk::SDK ${CMAKE_DL_LIBS})
add_test(NAME test_abi COMMAND test_abi $<TARGET_FILE:plugin_magpie_agent>)

add_executable(test_magpie_plugin tests/test_magpie_plugin.cpp)
target_link_libraries(test_magpie_plugin
    PRIVATE
        plugin_magpie_agent_core
        ${CMAKE_DL_LIBS}
)
target_compile_definitions(test_magpie_plugin PRIVATE
    MAGPIE_PLUGIN_SO_PATH="$<TARGET_FILE:plugin_magpie_agent>"
)
add_test(NAME test_magpie_plugin COMMAND test_magpie_plugin)
