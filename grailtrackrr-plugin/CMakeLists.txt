cmake_minimum_required(VERSION 3.29)
project(GrailTrackrr LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to a checkout of https://github.com/D2RLoader/PluginSDK
set(D2RL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk/PluginSDK" CACHE PATH "D2RLoader PluginSDK folder (build.ps1 downloads it here)")
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${D2RL_SDK_DIR}" d2rl-sdk)

add_library(GrailTrackrr SHARED src/grailtrackrr.cpp src/grailtrackrr.rc)
target_link_libraries(GrailTrackrr PRIVATE D2RLPlugin::D2RLPlugin shell32 ole32 winhttp)
set_target_properties(GrailTrackrr PROPERTIES OUTPUT_NAME "d2rl-grailtrackrr")
target_compile_definitions(GrailTrackrr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
	target_compile_options(GrailTrackrr PRIVATE /W3 /permissive- /EHsc /utf-8)
endif()

d2rlplugin_embed_config(TARGET GrailTrackrr FILE "${CMAKE_CURRENT_SOURCE_DIR}/grailtrackrr.toml")
