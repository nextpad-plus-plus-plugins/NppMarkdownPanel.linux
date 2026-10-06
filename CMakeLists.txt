cmake_minimum_required(VERSION 3.20)
project(NppMarkdownPanel VERSION 1.0.9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkitgtk-6.0)

# Sibling-repo headers: the Linux host's plugin contract + vendored Scintilla.
set(NPP_LINUX_DIR "${CMAKE_SOURCE_DIR}/../../nextpad-plus-plus-gtk4")
set(NPP_PLUGIN_INCLUDE "${NPP_LINUX_DIR}/src")
set(SCINTILLA_INCLUDE "${NPP_LINUX_DIR}/scintilla/include")

add_library(NppMarkdownPanel SHARED
    src/NppMarkdownPanel.cpp
)

target_include_directories(NppMarkdownPanel PRIVATE
    ${NPP_PLUGIN_INCLUDE}
    ${SCINTILLA_INCLUDE}
    src
)

target_link_libraries(NppMarkdownPanel PRIVATE
    PkgConfig::GTK4
    PkgConfig::WEBKIT)

# scintilla_view_send_message stays undefined at link time — it resolves at
# dlopen() against the host-loaded libscintilla.so (see PORTING_NOTES.md).
set_target_properties(NppMarkdownPanel PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "NppMarkdownPanel"
)

# Install into the user plugins dir the host scans. The runtime reads its
# JS/CSS/icons + help.md from <so_dir>/resources/ — without it the plugin
# fails silently at first render (marked.js missing), same as macOS.
set(PLUGIN_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/NppMarkdownPanel")
install(TARGETS NppMarkdownPanel DESTINATION "${PLUGIN_INSTALL_DIR}")
install(DIRECTORY resources/ DESTINATION "${PLUGIN_INSTALL_DIR}/resources")

# `cmake --build <dir> --target package_zip` → <dir>/NppMarkdownPanel.zip with
# the folder INSIDE the archive (Plugins Admin expects <Name>/<Name>.so).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/pkg/NppMarkdownPanel")
add_custom_target(package_zip
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/pkg/NppMarkdownPanel"
    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:NppMarkdownPanel>"
            "${CMAKE_SOURCE_DIR}/License.txt"
            "${CMAKE_BINARY_DIR}/pkg/NppMarkdownPanel/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources"
            "${CMAKE_BINARY_DIR}/pkg/NppMarkdownPanel/resources"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/NppMarkdownPanel.zip"
            --format=zip NppMarkdownPanel
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/pkg"
    DEPENDS NppMarkdownPanel
    COMMENT "Packaging NppMarkdownPanel.zip")
