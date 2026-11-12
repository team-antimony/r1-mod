@echo off
rem Builds r1-mod.dll and the r1-mod.exe launcher (x86) with the VS 2022 Build Tools.
rem   build.bat                 Release build -> build\bin\Win32\Release\
rem   build.bat debug           Debug build   -> build\bin\Win32\Debug\
rem   build.bat dll dev1_       only the DLL, renamed (e.g. dev1_r1-mod.dll while r1-mod.dll is loaded in the game)
setlocal
set CONFIG=Release
set TARGETS=
set PREMAKE_ARGS=
if /i "%~1"=="debug" set CONFIG=Debug
if /i "%~1"=="dll" (
    set TARGETS=/t:client
    if not "%~2"=="" set PREMAKE_ARGS=--dll-prefix=%~2
)
"%~dp0tools\premake5.exe" --file="%~dp0premake5.lua" %PREMAKE_ARGS% vs2022 >nul || exit /b 1
rem The VS 2022 Build Tools, else any Visual Studio with the C++ tools (vswhere: Community, Enterprise, CI runners).
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VCVARS%" if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARS%" x86 >nul || exit /b 1
msbuild "%~dp0build\r1-mod.sln" /nologo /v:minimal /m %TARGETS% /p:Configuration=%CONFIG% /p:Platform=Win32
exit /b %errorlevel%
