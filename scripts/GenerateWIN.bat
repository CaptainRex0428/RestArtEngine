@echo off
pushd ..
scripts\premake\premake5.exe --file=Build.lua vs2022
popd
pause
