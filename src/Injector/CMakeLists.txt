# ============================================================
# Injector — 3DMigoto Loader (注入工具)
# ============================================================
add_executable(Injector
    Injector.cpp
    stdafx.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ini_parser_lite.cpp
)

target_include_directories(Injector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/common
)

target_link_libraries(Injector PRIVATE
    version.lib
)

set_target_properties(Injector PROPERTIES
    OUTPUT_NAME "3DMigoto Loader"
    WIN32_EXECUTABLE FALSE
)

set_target_msvc_options(Injector)
set_target_common_definitions(Injector)

target_compile_definitions(Injector PRIVATE
    _CONSOLE
)
