project "DearImGui"
	kind "StaticLib"
	language "C++"
	cppdialect "C++20"

	files
	{
		"imgui/imconfig.h",
		"imgui/imgui.h",
		"imgui/imgui_internal.h",
		"imgui/imstb_rectpack.h",
		"imgui/imstb_textedit.h",
		"imgui/imstb_truetype.h",
		"imgui/imgui.cpp",
		"imgui/imgui_draw.cpp",
		"imgui/imgui_tables.cpp",
		"imgui/imgui_widgets.cpp",
		"imgui/backends/imgui_impl_win32.h",
		"imgui/backends/imgui_impl_win32.cpp",
		"imgui/backends/imgui_impl_dx11.h",
		"imgui/backends/imgui_impl_dx11.cpp"
	}

	includedirs
	{
		"imgui",
		"imgui/backends"
	}

	links
	{
		"d3d11",
		"dxgi",
		"d3dcompiler",
		"dwmapi"
	}

	location (LocationDir)
	targetdir (TargetDir)
	objdir (ObjectDir)

	filter "system:windows"
		systemversion "latest"
		defines { "_WINDOWS" }

	filter "configurations:Debug"
		runtime "Debug"
		symbols "On"
		defines { "_DEBUG" }

	filter "configurations:Release"
		runtime "Release"
		optimize "On"
		symbols "On"
		defines { "_RELEASE", "NDEBUG" }

	filter "configurations:Dist"
		runtime "Release"
		optimize "On"
		symbols "Off"
		defines { "_DIST", "NDEBUG" }
