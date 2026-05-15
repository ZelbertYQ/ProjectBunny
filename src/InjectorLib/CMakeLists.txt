# ============================================================
# InjectorLib — 注入器库
# Win32: EXE, x64: DLL
# ============================================================

# 根据架构选择输出类型: Win32 → EXE, x64 → DLL
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(InjectorLib
        Injector.cpp
        ${CMAKE_SOURCE_DIR}/src/common/ini_parser_lite.cpp
    )
    set_target_properties(InjectorLib PROPERTIES
        OUTPUT_NAME "3dmloader"
    )
    target_compile_definitions(InjectorLib PRIVATE
        _CONSOLE
    )
else()
    add_library(InjectorLib SHARED
        Injector.cpp
        ${CMAKE_SOURCE_DIR}/src/common/ini_parser_lite.cpp
    )
    set_target_properties(InjectorLib PROPERTIES
        OUTPUT_NAME "3dmloader"
    )
    target_compile_definitions(InjectorLib PRIVATE
        _USRDLL
    )
endif()

target_include_directories(InjectorLib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/common
)

target_link_libraries(InjectorLib PRIVATE
    version.lib
)

set_target_msvc_options(InjectorLib)
set_target_common_definitions(InjectorLib)
