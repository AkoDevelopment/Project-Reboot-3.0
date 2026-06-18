#pragma once

// Project Ocean: allows the dedicated server's startup behavior to be configured
// via Fortnite launch arguments instead of always using the hardcoded defaults in
// gui.h / globals.h. This lets an external launcher (or automated script) configure
// the server non-interactively, since there's no one available to click the ImGui
// checkboxes/sliders on an automated/headless box.

#include <Windows.h>
#include <string>
#include <algorithm>

#include "gui.h"
#include "globals.h"

inline bool FindLaunchArgValue(const std::wstring& commandLine, const std::wstring& key, std::wstring& outValue)
{
	size_t pos = commandLine.find(key);
	if (pos == std::wstring::npos) return false;

	pos += key.length();
	size_t end = commandLine.find(L' ', pos);
	if (end == std::wstring::npos) end = commandLine.length();

	outValue = commandLine.substr(pos, end - pos);
	return true;
}

inline void ApplyLaunchArgOverrides()
{
	std::wstring commandLine = GetCommandLineW();
	std::wstring value;

	// -RebootSecondsUntilTravel=30
	if (FindLaunchArgValue(commandLine, L"-RebootSecondsUntilTravel=", value))
	{
		try { SecondsUntilTravel = std::stoi(value); }
		catch (...) {}
	}

	// -RebootWarmupPlayers=1
	if (FindLaunchArgValue(commandLine, L"-RebootWarmupPlayers=", value))
	{
		try { WarmupRequiredPlayerCount = std::stoi(value); }
		catch (...) {}
	}

	// -RebootNoMCP=0 or -RebootNoMCP=1
	if (FindLaunchArgValue(commandLine, L"-RebootNoMCP=", value))
	{
		Globals::bNoMCP = (value == L"1");
	}

	// -RebootPlaylist=/Game/Athena/Playlists/Playlist_DefaultDuo.Playlist_DefaultDuo
	// Without this the server always loads whatever PlaylistName defaults to
	// regardless of which playlist the backend registered it under.
	if (FindLaunchArgValue(commandLine, L"-RebootPlaylist=", value))
	{
		PlaylistName = std::string(value.begin(), value.end());
	}

	// The backend's matchLifecycleManager is the sole intended trigger for starting
	// the bus (via the "startBus" command from httpreporter.h) -- explicitly keep the
	// native auto-bus-start path (FortGameModeAthena.cpp's AutoBusStartSeconds timer,
	// gated on Globals::bAutoRestart) off so it can never race the backend's countdown,
	// regardless of future UI/default changes. It is already false by default today.
	Globals::bAutoRestart = false;
}
