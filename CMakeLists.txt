cmake_minimum_required(VERSION 3.30)

find_package(Git QUIET)
set(_git_desc "")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always --match "v*"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_desc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

set(_maj 1)
set(_min 0)
set(_pat 0)
if(_git_desc MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(_maj "${CMAKE_MATCH_1}")
    set(_min "${CMAKE_MATCH_2}")
    set(_pat "${CMAKE_MATCH_3}")
endif()

project(obs-multistream-plugin VERSION "${_maj}.${_min}.${_pat}" LANGUAGES CXX)

set(_plugin_ui "${_git_desc}")
if(_plugin_ui STREQUAL "")
    set(_plugin_ui "v${PROJECT_VERSION}")
endif()

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OBS_SDK_HINT "" CACHE PATH "Path to OBS Studio development SDK/prefix that contains libobsConfig.cmake")
if(OBS_SDK_HINT)
    list(PREPEND CMAKE_PREFIX_PATH "${OBS_SDK_HINT}")
endif()

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 COMPONENTS Widgets REQUIRED)

set(_plugin_sources
    src/plugin-main.cpp
    src/destination_rules.cpp
    src/stream_key_storage.cpp
)
if(WIN32)
    list(APPEND _plugin_sources src/version.rc)
endif()

add_library(obs-multistream-plugin MODULE ${_plugin_sources})

if(WIN32)
    target_compile_definitions(obs-multistream-plugin PRIVATE
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        PLUGIN_VERSION_BUILD=0
    )
    set_source_files_properties(src/version.rc PROPERTIES COMPILE_DEFINITIONS
        "PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR};PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH};PLUGIN_VERSION_BUILD=0")
    target_link_libraries(obs-multistream-plugin PRIVATE crypt32)
endif()

if(APPLE)
    find_library(_multistream_corefoundation CoreFoundation REQUIRED)
    find_library(_multistream_security Security REQUIRED)
    target_link_libraries(obs-multistream-plugin PRIVATE "${_multistream_corefoundation}" "${_multistream_security}")
endif()

if(UNIX AND NOT APPLE)
    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        target_link_libraries(obs-multistream-plugin PRIVATE OpenSSL::Crypto)
        target_compile_definitions(obs-multistream-plugin PRIVATE STREAM_KEY_USE_OPENSSL=1)
    endif()
endif()

target_include_directories(obs-multistream-plugin PRIVATE src)

target_link_libraries(obs-multistream-plugin PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Widgets)

target_compile_features(obs-multistream-plugin PRIVATE cxx_std_17)

target_compile_definitions(obs-multistream-plugin PRIVATE
    "PLUGIN_UI_VERSION_STRING=\"${_plugin_ui}\""
)

if(MSVC)
    target_compile_options(obs-multistream-plugin PRIVATE /W4 /permissive-)
else()
    target_compile_options(obs-multistream-plugin PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(obs-multistream-plugin PROPERTIES
    OUTPUT_NAME "obs-multistream-plugin"
    PREFIX ""
)

install(TARGETS obs-multistream-plugin
    RUNTIME DESTINATION obs-plugins
    LIBRARY DESTINATION obs-plugins
)

enable_testing()

add_executable(destination-rules-tests
    tests/destination_rules_tests.cpp
    src/destination_rules.cpp
)
target_include_directories(destination-rules-tests PRIVATE src)
target_compile_features(destination-rules-tests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(destination-rules-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(destination-rules-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME destination-rules-tests COMMAND destination-rules-tests)
