@echo off
rem Writes build\r1-mod.sln for Visual Studio 2022 from premake5.lua.
"%~dp0tools\premake5.exe" --file="%~dp0premake5.lua" vs2022
