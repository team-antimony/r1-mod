-- r1-mod build script: `generate.bat` writes build\r1-mod.sln (Visual Studio 2022); `build.bat` also compiles it.

newoption {
	trigger = "dll-prefix",
	value = "PREFIX",
	description = "Prefix for the DLL name (e.g. dev1_ while r1-mod.dll is loaded in the game)"
}

workspace "r1-mod"
	location "build"
	configurations { "Debug", "Release" }
	platforms { "Win32" }
	startproject "launcher"

	architecture "x86"
	staticruntime "On"
	characterset "MBCS"
	warnings "Default"
	defines { "_CRT_SECURE_NO_WARNINGS" }

	targetdir "build/bin/%{cfg.platform}/%{cfg.buildcfg}"
	objdir "build/obj/%{prj.name}/%{cfg.platform}/%{cfg.buildcfg}"

	filter "configurations:Debug"
		symbols "On"
		optimize "Off"
		defines { "DEBUG" }

	filter "configurations:Release"
		symbols "Off"
		optimize "Speed"
		defines { "NDEBUG" }

	filter {}

-- Dear ImGui 1.92.7 (MIT), vendored in deps\imgui with the DX9 and Win32 backends
project "imgui"
	kind "StaticLib"
	language "C++"
	files { "deps/imgui/*.cpp", "deps/imgui/*.h" }
	includedirs { "deps/imgui" }

-- r1-mod.dll: injected into codoMP_client_shipRetail_loosefs_v2.exe by the launcher
project "client"
	kind "SharedLib"
	language "C++"
	targetname((_OPTIONS["dll-prefix"] or "") .. "r1-mod")
	files { "src/client/**.c", "src/client/**.cpp", "src/client/**.h", "src/common/**.h" }
	includedirs { "src/client", "src/common", "deps/imgui" }
	links { "imgui", "ws2_32", "user32" }
	-- GSC mods for cx_mod (src\client\component\mod.c) go next to the DLL
	postbuildcommands { '{COPYDIR} "%{wks.location}/../mods" "%{cfg.targetdir}/mods"' }

-- r1-mod.exe: starts the backend and the game, injects r1-mod.dll at the entry point
project "launcher"
	kind "ConsoleApp"
	language "C"
	targetname "r1-mod"
	files { "src/launcher/**.c", "src/launcher/**.h", "src/common/**.h" }
	includedirs { "src/common" }
	links { "ws2_32" }
	-- the DLL and the exe share a target name and folder; keep their debug databases apart
	filter "configurations:Debug"
		symbolspath "%{cfg.targetdir}/r1-mod-launcher.pdb"
	filter {}
