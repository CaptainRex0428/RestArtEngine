@echo off
setlocal
pushd "%~dp0.."
if errorlevel 1 (
    echo Failed to enter the RestArtEngine repository root.
    exit /b 1
)
scripts\premake\premake5.exe --file=Build.lua vs2022
set "exitCode=%errorlevel%"
popd
exit /b %exitCode%
