-- Single source of truth for the MSBuild configurations. Build.lua feeds this
-- list to `configurations`, and the Rider run configuration below emits one
-- entry per item, so a new configuration cannot leave the dropdown behind.
EngineConfigurations = { "Debug", "Release", "Dist" }

local run_config_dir = ".idea/.idea.RestArtEngine/.idea/runConfigurations"

-- Editor files are machine-local and may be cleaned or locked at any time.
-- Generation degrades to a warning instead of aborting the whole premake run,
-- because an interrupted run leaves the solution/project files stale.
local function ensure_directory(path)
    if os.isdir(path) then return true end
    local created, err = os.mkdir(path)
    if not created and not os.isdir(path) then
        print("warning: cannot create " .. path .. ": " .. tostring(err))
        return false
    end
    return true
end

local function read_text(filename)
    local file = io.open(filename, "r")
    if not file then return nil end
    local text = file:read("*a")
    file:close()
    return text:gsub("\r\n", "\n")
end

local function write_if_changed(filename, xml)
    -- Skip identical writes so Rider does not reload the run configuration on
    -- every premake run.
    local normalized = xml:gsub("\r\n", "\n")
    if read_text(filename) == normalized then return true end
    local file, err = io.open(filename, "w")
    if not file then
        print("warning: cannot write " .. filename .. ": " .. tostring(err))
        return false
    end
    file:write(normalized)
    file:close()
    return true
end

local function write_rider_config(filename, xml)
    if not ensure_directory(run_config_dir) then return false end
    return write_if_changed(run_config_dir .. "/" .. filename, xml)
end

function add_rider_run_configurations()
    -- These are solution-scoped project configurations, not personal workspace state.
    -- Keep MSBuild macros so the selected solution configuration chooses the binary.
    local launch = {
        '<component name="ProjectRunConfigurationManager">',
        '  <configuration default="false" name="RestArtEngine" type="CppProject" factoryName="C++ Project">'
    }
    for index, config in ipairs(EngineConfigurations) do
        table.insert(launch, string.format([[
    <configuration_%d setup="1">
      <option name="CONFIGURATION" value="%s" />
      <option name="PLATFORM" value="x64" />
      <option name="CURRENT_LAUNCH_PROFILE" value="Local" />
      <option name="EXE_PATH" value="$(LocalDebuggerCommand)" />
      <option name="PROGRAM_PARAMETERS" value="$(LocalDebuggerCommandArguments)" />
      <option name="WORKING_DIRECTORY" value="$(LocalDebuggerWorkingDirectory)" />
      <option name="PASS_PARENT_ENVS" value="1" />
      <option name="USE_EXTERNAL_CONSOLE" value="0" />
      <option name="TERMINAL_INTERACTION_BEHAVIOR" value="AUTO_DETECT" />
      <option name="PROJECT_FILE_PATH" value="$PROJECT_DIR$/settings/RestArtEngine.vcxproj" />
    </configuration_%d>]], index, config, index))
    end
    table.insert(launch, [[
    <option name="DEFAULT_PROJECT_PATH" value="$PROJECT_DIR$/settings/RestArtEngine.vcxproj" />
    <option name="PROJECT_FILE_PATH" value="$PROJECT_DIR$/settings/RestArtEngine.vcxproj" />
    <option name="AUTO_SELECT_PRIORITY" value="0" />
    <method v="2">
      <option name="Build" />
    </method>
  </configuration>
</component>
]])
    write_rider_config("RestArtEngine.xml", table.concat(launch, "\n"))

    local xml = [[
<component name="ProjectRunConfigurationManager">
  <configuration default="false" name="RefreshConfig" type="ShConfigurationType">
    <option name="SCRIPT_TEXT" value="scripts\premake\premake5.exe --file=Build.lua vs2022" />
    <option name="INDEPENDENT_SCRIPT_PATH" value="true" />
    <option name="SCRIPT_PATH" value="" />
    <option name="SCRIPT_OPTIONS" value="" />
    <option name="INDEPENDENT_SCRIPT_WORKING_DIRECTORY" value="true" />
    <option name="SCRIPT_WORKING_DIRECTORY" value="$PROJECT_DIR$" />
    <option name="INDEPENDENT_INTERPRETER_PATH" value="true" />
    <option name="INTERPRETER_PATH" value="powershell.exe" />
    <option name="INTERPRETER_OPTIONS" value="" />
    <option name="EXECUTE_IN_TERMINAL" value="true" />
    <option name="EXECUTE_SCRIPT_FILE" value="false" />
    <envs />
    <method v="2" />
  </configuration>
</component>
]]

    -- Migrate only our unchanged legacy helper. A customized helper belongs to
    -- the user and must not be overwritten or duplicated under the same name.
    local legacy = ".run/RefreshConfig.run.xml"
    local previous = read_text(legacy)
    if not previous or previous == xml:gsub("\r\n", "\n") then
        write_rider_config("RefreshConfig.xml", xml)
        if previous then
            local removed, err = os.remove(legacy)
            if not removed then print("warning: cannot remove " .. legacy .. ": " .. tostring(err)) end
        end
    end
end

function add_fork_custom_commands()
  local fork_dir = ".fork"
  if not os.isdir(fork_dir) then
      os.mkdir(fork_dir)
  end

  local json = [[
[
  {
    "version" : 2
  },
  {
    "action" : {
      "script" : "echo ${repo:name} status:\n\ncursor .",
      "showOutput" : true,
      "type" : "sh",
      "waitForExit" : true
    },
    "name" : "Open With Cursor",
    "target" : "repository"
  },
  {
    "action" : {
      "script" : "echo ${repo:name} status:\n\ncode .",
      "showOutput" : true,
      "type" : "sh",
      "waitForExit" : true
    },
    "name" : "Open With VSCode",
    "target" : "repository"
  },
  {
    "action" : {
      "script" : "echo ${repo:name} status:\n\nscripts/premake/premake5.exe --file=Build.lua vs2022",
      "showOutput" : true,
      "type" : "sh",
      "waitForExit" : true
    },
    "name" : "Refresh Solution Settings",
    "target" : "repository"
  }
]
  ]]

  local file = io.open(fork_dir .. "/custom-commands.json", "w")
  file:write(json)
  file:close()
end
