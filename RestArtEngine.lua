project "RestArtEngine"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++20"

	files
	{
		"app/**.cpp",
		"app/**.h",
		"src/**.cpp",
		"src/**.h",
		"include/RestArtEngine/**.h",
		-- HLSL sources are listed so they can be edited from the solution; the
		-- build does not compile them (they are loaded and compiled at runtime).
		"Shaders/**.hlsl",
		"Shaders/**.hlsli",
		"vendor/ImGuizmo/src/ImGuizmo.cpp",
		"vendor/ImGuizmo/src/ImGuizmo.h",
		"RestArtEngine.rc"
	}

	includedirs
	{
		ProjIncludeDir["RestArtEngine"],
		DepIncludeDir["nlohmann"],
		DepIncludeDir["imgui"],
		DepIncludeDir["imgui_backends"],
		DepIncludeDir["ImGuizmo"]
	}

	-- Resolve Win32/DX11 imports at the executable link, not inside DearImGui.lib.
	links
	{
		"DearImGui",
		"d3d11",
		"dxgi",
		"d3dcompiler",
		"dwmapi",
		"comdlg32"
	}

	defines
	{
		"UNICODE",
		"_UNICODE",
		"_CRT_SECURE_NO_WARNINGS"
	}

	warnings "Extra"
	buildoptions { "/permissive-" }
	flags { "MultiProcessorCompile" }

	-- HLSL is edited from the solution but compiled at runtime by the engine.
	-- Without this premake maps .hlsl to FxCompile and the build tries to
	-- compile it with no shader settings.
	filter "files:Shaders/**.hlsl or files:Shaders/**.hlsli"
		buildaction "None"
	filter {}

	-- ImGuizmo is compiled under /W4; silence its signed/unsigned noise.
	filter "files:**/ImGuizmo.cpp"
		disablewarnings { "4245" }
	filter {}

	location (LocationDir)
	targetdir (EXEDir)
	objdir (ObjectDir)
	debugcommand (EXEDir .. "/RestArtEngine.exe")
	debugdir (EXEDir)

	filter "system:windows"
		systemversion "latest"
		defines { "_WINDOWS", "_CONSOLE" }

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
