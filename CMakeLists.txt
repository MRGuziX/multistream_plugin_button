cmake_minimum_required(VERSION 3.30)

project(obs-multistream-plugin VERSION 0.1.0 LANGUAGES CXX)

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

add_library(obs-multistream-plugin MODULE
    src/plugin-main.cpp
)

target_link_libraries(obs-multistream-plugin PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Widgets)

target_compile_features(obs-multistream-plugin PRIVATE cxx_std_17)

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
