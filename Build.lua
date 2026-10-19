include "Directory.lua"
include "Dependencies.lua"
include "Config.lua"

workspace "RestArtEngine"
	architecture "x64"
	startproject "RestArtEngine"
	configurations
	{
		"Debug",
		"Release",
        "Dist"
	}

    filter "system:windows"
    buildoptions { "/EHsc", "/Zc:preprocessor", "/Zc:__cplusplus", "/utf-8" }

	add_rider_refreshconfig()
	add_fork_custom_commands()

group ""
	include "RestArtEngine.lua"

group "Dependencies"
	include "vendor/imgui.lua"
