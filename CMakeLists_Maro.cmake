cmake_minimum_required(VERSION 3.15)
project(MaroArena LANGUAGES C CXX)

file(TO_CMAKE_PATH "${COCOS2DX_ROOT_PATH}" COCOS2DX_ROOT_PATH)
file(TO_CMAKE_PATH "${MARO_SOURCE_DIR}" MARO_SOURCE_DIR)

if(NOT MSVC OR NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Maro Arena requires a Win32 MSVC build.")
endif()

if(NOT EXISTS "${COCOS2DX_ROOT_PATH}/cocos/CMakeLists.txt" OR
   NOT EXISTS "${COCOS2DX_ROOT_PATH}/external/CMakeLists.txt")
    message(FATAL_ERROR "Set COCOS2DX_ROOT_PATH to Cocos2d-x 4.0 with its external dependencies installed.")
endif()

set(CMAKE_MODULE_PATH "${COCOS2DX_ROOT_PATH}/cmake/Modules")
set(BUILD_SHARED_LIBS OFF)
set(BUILD_LUA_LIBS OFF)
set(BUILD_EDITOR_COCOSTUDIO OFF CACHE BOOL "" FORCE)
set(BUILD_EDITOR_SPINE OFF CACHE BOOL "" FORCE)
include(CocosBuildSet)
add_subdirectory("${COCOS2DX_ROOT_PATH}/cocos" "${ENGINE_BINARY_PATH}/cocos/core")

add_executable(MaroArena WIN32
    "${MARO_SOURCE_DIR}/Source/Main_Maro.cpp"
    "${MARO_SOURCE_DIR}/Source/Arena_Maro.cpp"
    "${MARO_SOURCE_DIR}/Source/Battle_Maro.cpp"
    "${MARO_SOURCE_DIR}/Source/Lan_Maro.cpp"
    "${MARO_SOURCE_DIR}/Source/Menu_Maro.cpp"
    "${MARO_SOURCE_DIR}/Source/Ui_Maro.cpp"
)
target_link_libraries(MaroArena PRIVATE cocos2d comctl32 ws2_32 iphlpapi)
target_compile_features(MaroArena PRIVATE cxx_std_17)
target_compile_options(MaroArena PRIVATE /utf-8)
target_compile_definitions(MaroArena PRIVATE NOMINMAX)
set_target_properties(MaroArena PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
cocos_copy_target_dll(MaroArena)

add_custom_target(MaroAssets ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${MARO_SOURCE_DIR}/Assets" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Assets"
    VERBATIM
)
add_dependencies(MaroArena MaroAssets)

install(TARGETS MaroArena RUNTIME DESTINATION . COMPONENT MaroArenaRuntime)
install(FILES
    "${COCOS2DX_ROOT_PATH}/external/win32-specific/gles/prebuilt/glew32.dll"
    "${COCOS2DX_ROOT_PATH}/external/win32-specific/zlib/prebuilt/zlib1.dll"
    "${MARO_SOURCE_DIR}/ThirdParty_Maro.license"
    DESTINATION . COMPONENT MaroArenaRuntime
)
install(DIRECTORY "${MARO_SOURCE_DIR}/Assets" DESTINATION . COMPONENT MaroArenaRuntime)
