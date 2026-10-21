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
