# ============================================================
# BunnyCore - shared config and utility layer
# ============================================================
add_library(BunnyCore STATIC
    IniDocument.cpp
    MigotoIniLoader.cpp
    MigotoConfig.cpp
    MigotoShaderOverride.cpp
    MigotoTextureOverride.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ini_parser_lite.cpp
)

target_include_directories(BunnyCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(BunnyCore PUBLIC
    Shlwapi.lib
)

target_compile_definitions(BunnyCore PUBLIC
    UNICODE
    _UNICODE
)

set_target_msvc_options(BunnyCore)
set_target_common_definitions(BunnyCore)
