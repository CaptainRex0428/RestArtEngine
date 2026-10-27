cmake_minimum_required(VERSION 3.24)
project(RestArtEngine VERSION 0.2.0 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "RestArtEngine currently targets Windows and DirectX 11.")
endif()

# Vendored Dear ImGui (Win32/DX11 backend) as its own library.
add_library(DearImGui STATIC
    vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp vendor/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(DearImGui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui>
    $<INSTALL_INTERFACE:include/RestArtEngine/ThirdParty/imgui>)
target_include_directories(DearImGui PRIVATE vendor/imgui/backends)
# These are consumer link requirements; do not merge import libraries into the archive.
target_link_libraries(DearImGui INTERFACE d3d11 dxgi d3dcompiler dwmapi)

# The single engine executable: project manager, editor, and renderer.
# It owns the project selection screen and editor entry point; users build and
# run `RestArtEngine` directly. The project manager doubles as the version selector,
# so there is no separate project-manager target.
add_executable(RestArtEngine
    app/MinimalApp.cpp
    app/EditorSmoke.cpp
    app/GarbageCollectionSmoke.cpp
    src/Core/Engine.cpp
    src/Core/GarbageCollector.cpp
    src/Renderer/RenderBackend.cpp
    src/Renderer/SceneRenderer.cpp
    src/Renderer/SelectionOutline.cpp
    src/Assets/AssetLibrary.cpp
    src/Assets/AssetRegistry.cpp
    src/Scene/World.cpp
    src/Project/Project.cpp
    src/ImGui/ImGuiLayer.cpp
    src/ImGui/EditorTheme.cpp
    src/ImGui/WindowChrome.cpp
    src/Editor/EditorDockManager.cpp
    src/Editor/EditorCommands.cpp
    src/Editor/SceneEditor.cpp
    src/Editor/UndoStack.cpp
    src/Editor/ViewportInteraction.cpp
    src/Editor/LoadingScreen.cpp
    vendor/ImGuizmo/src/ImGuizmo.cpp
    RestArtEngine.rc)
# HLSL sources are listed for editing in IDEs; the build never compiles them.
file(GLOB_RECURSE RENDER_SHADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsli")
target_sources(RestArtEngine PRIVATE ${RENDER_SHADERS})
set_source_files_properties(${RENDER_SHADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Shaders" PREFIX "Shaders" FILES ${RENDER_SHADERS})
target_include_directories(RestArtEngine PRIVATE
    include
    vendor/nlohmann/single_include
    vendor/imgui
    vendor/imgui/backends
    vendor/ImGuizmo/src)
target_link_libraries(RestArtEngine PRIVATE DearImGui d3d11 dxgi d3dcompiler dwmapi comdlg32)
target_compile_definitions(RestArtEngine PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(RestArtEngine PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(RestArtEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()
# Vendored ImGuizmo is compiled under /W4; silence its signed/unsigned noise.
set_source_files_properties(vendor/ImGuizmo/src/ImGuizmo.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4245>")
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT RestArtEngine)

include(CTest)
if(BUILD_TESTING)
    add_executable(GarbageCollectorTests tests/GarbageCollectorTests.cpp src/Core/GarbageCollector.cpp)
    target_include_directories(GarbageCollectorTests PRIVATE include vendor/nlohmann/single_include)
    target_compile_definitions(GarbageCollectorTests PRIVATE UNICODE _UNICODE NOMINMAX)
    add_test(NAME GarbageCollectorTests COMMAND GarbageCollectorTests)
    add_executable(LoadingScreenTests tests/LoadingScreenTests.cpp src/Editor/LoadingScreen.cpp)
    target_include_directories(LoadingScreenTests PRIVATE include)
    target_link_libraries(LoadingScreenTests PRIVATE DearImGui windowscodecs ole32)
    add_test(NAME LoadingScreenTests COMMAND LoadingScreenTests "${CMAKE_CURRENT_SOURCE_DIR}/assets/load/load.png")
endif()

include(GNUInstallDirs)
install(TARGETS RestArtEngine DearImGui EXPORT RestArtEngineTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES Engine.raeengine DESTINATION .)
install(DIRECTORY Shaders/ DESTINATION Shaders)
install(DIRECTORY Projects/Sandbox/ DESTINATION Projects/Sandbox)
install(DIRECTORY assets/load/ DESTINATION assets/load)
install(DIRECTORY vendor/imgui/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/RestArtEngine/ThirdParty/imgui
    FILES_MATCHING PATTERN "*.h")
install(FILES vendor/imgui/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/RestArtEngine/licenses RENAME imgui-LICENSE.txt)
install(EXPORT RestArtEngineTargets FILE RestArtEngineTargets.cmake NAMESPACE RestArtEngine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RestArtEngine)
