include "Directory.lua"
include "Dependencies.lua"
include "Config.lua"

workspace "RestArtEngine"
	architecture "x64"
	startproject "RestArtEngine"
	-- Shared with Config.lua so the Rider run configuration always mirrors the
	-- solution configurations.
	configurations(EngineConfigurations)

    filter "system:windows"
    buildoptions { "/EHsc", "/Zc:preprocessor", "/Zc:__cplusplus", "/utf-8" }

	if _ACTION == "vs2022" then
		add_rider_run_configurations()
	end
	add_fork_custom_commands()

group ""
	include "RestArtEngine.lua"

group "Dependencies"
	include "vendor/imgui.lua"
