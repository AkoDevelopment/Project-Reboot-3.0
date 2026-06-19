#pragma once

// Project Ocean: periodically reports live match state (player count, alive count,
// game phase) to the backend over HTTP, and executes whatever command the backend
// sends back in the response ("startBus" or "restart"). This is the only outbound
// network call this mod makes; everything else is local Unreal Engine state/replication.
//
// The backend is the sole authority over bus-start/restart timing -- this file does
// not make any local decisions about when those should happen, it just reports state
// and obeys commands.

#pragma comment(lib, "winhttp.lib")

#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <format>

#include "reboot.h"
#include "gui.h"
#include "globals.h"
#include "FortGameModeAthena.h"
#include "FortGameStateAthena.h"
#include "launchargs.h"

inline std::wstring GReportBackendHost;
inline int GReportBackendPort = 80;
inline std::wstring GReportServerId = L"unknown";
inline std::wstring GReportSecret;
inline int GReportIntervalMs = 2000;

inline bool ParseBackendUrl(const std::wstring& url, std::wstring& outHost, int& outPort)
{
	std::wstring rest = url;
	const std::wstring httpPrefix = L"http://";
	if (rest.rfind(httpPrefix, 0) == 0) rest = rest.substr(httpPrefix.length());

	size_t colonPos = rest.find(L':');
	if (colonPos == std::wstring::npos)
	{
		outHost = rest;
		outPort = 80;
		return !outHost.empty();
	}

	outHost = rest.substr(0, colonPos);
	try { outPort = std::stoi(rest.substr(colonPos + 1)); }
	catch (...) { outPort = 80; }

	return !outHost.empty();
}

inline std::string WideToUtf8(const std::wstring& wide)
{
	if (wide.empty()) return "";
	int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.length(), nullptr, 0, nullptr, nullptr);
	std::string result(size, 0);
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.length(), result.data(), size, nullptr, nullptr);
	return result;
}

inline std::string GamePhaseStepToString(EAthenaGamePhaseStep step)
{
	switch (step)
	{
		case EAthenaGamePhaseStep::Setup: return "Setup";
		case EAthenaGamePhaseStep::Warmup: return "Warmup";
		case EAthenaGamePhaseStep::GetReady: return "GetReady";
		case EAthenaGamePhaseStep::BusLocked: return "BusLocked";
		case EAthenaGamePhaseStep::BusFlying: return "BusFlying";
		case EAthenaGamePhaseStep::StormForming: return "StormForming";
		case EAthenaGamePhaseStep::StormHolding: return "StormHolding";
		case EAthenaGamePhaseStep::StormShrinking: return "StormShrinking";
		case EAthenaGamePhaseStep::Countdown: return "Countdown";
		case EAthenaGamePhaseStep::FinalCountdown: return "FinalCountdown";
		case EAthenaGamePhaseStep::EndGame: return "EndGame";
		default: return "None";
	}
}

// Extracts a top-level string value for "command" out of a tiny, known-shape JSON
// response like {"command":"startBus"}. Not a general JSON parser -- mirrors the
// substring-search approach already used by FindLaunchArgValue in launchargs.h.
inline std::string ExtractCommand(const std::string& json)
{
	const std::string key = "\"command\"";
	size_t pos = json.find(key);
	if (pos == std::string::npos) return "none";

	pos = json.find(':', pos);
	if (pos == std::string::npos) return "none";

	size_t firstQuote = json.find('"', pos + 1);
	if (firstQuote == std::string::npos) return "none";

	size_t secondQuote = json.find('"', firstQuote + 1);
	if (secondQuote == std::string::npos) return "none";

	return json.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

inline std::string SendStatusReport(int playerCount, int aliveCount, const std::string& phase, bool joinable)
{
	HINTERNET hSession = WinHttpOpen(L"ProjectOceanReporter/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) return "none";

	// Keep a stuck/unreachable backend from hanging this thread for a long time --
	// 5s is generous for a LAN/VPN call but bounded.
	WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

	HINTERNET hConnect = WinHttpConnect(hSession, GReportBackendHost.c_str(), (INTERNET_PORT)GReportBackendPort, 0);
	if (!hConnect)
	{
		WinHttpCloseHandle(hSession);
		return "none";
	}

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/internal/server/report",
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
	if (!hRequest)
	{
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return "none";
	}

	std::string body = std::format(
		"{{\"serverId\":\"{}\",\"playerCount\":{},\"aliveCount\":{},\"phase\":\"{}\",\"joinable\":{}}}",
		WideToUtf8(GReportServerId), playerCount, aliveCount, phase, joinable ? "true" : "false");

	std::wstring headers = L"Content-Type: application/json\r\nX-Internal-Secret: " + GReportSecret;

	std::string result = "none";
	BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1L,
		(LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);

	if (sent && WinHttpReceiveResponse(hRequest, nullptr))
	{
		std::string responseBody;
		DWORD available = 0;
		while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
		{
			std::string chunk(available, 0);
			DWORD read = 0;
			if (!WinHttpReadData(hRequest, chunk.data(), available, &read)) break;
			chunk.resize(read);
			responseBody += chunk;
		}

		result = ExtractCommand(responseBody);
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	return result;
}

inline void ExecuteBackendCommand(const std::string& command)
{
	if (command == "startBus")
	{
		LOG_INFO(LogDev, "[ProjectOcean] Backend commanded startBus");

		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());
		if (!GameMode) return;

		auto GameState = Cast<AFortGameStateAthena>(GameMode->GetGameState());
		if (!GameState) return;

		bStartedBus = true;
		AmountOfPlayersWhenBusStart = GameState->GetPlayersLeft();

		if (Globals::bLateGame.load())
		{
			// LateGameThread already calls StartAircraftPhase() itself once it's
			// repositioned the aircraft over the shrunk zone -- this is the same
			// immediate trigger the ImGui "Start Bus" button uses for lategame.
			CreateThread(0, 0, LateGameThread, 0, 0, 0);
		}
		else
		{
			// Same as the ImGui "Start Bus Countdown" button: this sets up a real
			// 10s native countdown instead of an instant StartAircraftPhase() call,
			// so players see the same "match starting in 10..." countdown a human
			// clicking the button would have produced.
			static auto WarmupCountdownEndTimeOffset = GameState->GetOffset("WarmupCountdownEndTime");
			static auto WarmupCountdownDurationOffset = GameMode->GetOffset("WarmupCountdownDuration");
			static auto WarmupEarlyCountdownDurationOffset = GameMode->GetOffset("WarmupEarlyCountdownDuration");

			float TimeSeconds = GameState->GetServerWorldTimeSeconds();
			float Duration = 10;

			GameState->Get<float>(WarmupCountdownEndTimeOffset) = TimeSeconds + Duration;
			GameMode->Get<float>(WarmupCountdownDurationOffset) = Duration;
			GameMode->Get<float>(WarmupEarlyCountdownDurationOffset) = Duration;
		}
	}
	else if (command == "restart")
	{
		LOG_INFO(LogDev, "[ProjectOcean] Backend commanded restart");
		Restart();
	}
}

inline DWORD WINAPI HttpReportLoop(LPVOID)
{
	while (true)
	{
		Sleep(GReportIntervalMs);

		try
		{
			UWorld* World = GetWorld();
			if (!World) continue;

			auto GameMode = Cast<AFortGameModeAthena>(World->GetGameMode());
			if (!GameMode) continue;

			auto GameState = GameMode->GetGameStateAthena();
			if (!GameState) continue;

			int playerCount = World->GetNetDriver() ? World->GetNetDriver()->GetClientConnections().Num() : 0;
			int aliveCount = GameState->GetPlayersLeft();
			std::string phase = GamePhaseStepToString(GameState->GetGamePhaseStep());
			bool joinable = Globals::bStartedListening;

			std::string command = SendStatusReport(playerCount, aliveCount, phase, joinable);
			ExecuteBackendCommand(command);
		}
		catch (...)
		{
			// Keep the loop alive even if a single report cycle fails (e.g. transient
			// network error, or GameState not ready yet during early startup).
		}
	}

	return 0;
}

inline void StartHttpReportLoop()
{
	std::wstring commandLine = GetCommandLineW();
	std::wstring backendUrl;

	if (!FindLaunchArgValue(commandLine, L"-RebootBackendUrl=", backendUrl)) return;
	if (!ParseBackendUrl(backendUrl, GReportBackendHost, GReportBackendPort)) return;

	std::wstring value;
	if (FindLaunchArgValue(commandLine, L"-RebootServerId=", value)) GReportServerId = value;
	if (FindLaunchArgValue(commandLine, L"-RebootReportSecret=", value)) GReportSecret = value;
	if (FindLaunchArgValue(commandLine, L"-RebootReportIntervalMs=", value))
	{
		try { GReportIntervalMs = std::stoi(value); }
		catch (...) {}
	}

	LOG_INFO(LogDev, "[ProjectOcean] Starting backend report loop -> {}:{}", WideToUtf8(GReportBackendHost), GReportBackendPort);

	CreateThread(nullptr, 0, HttpReportLoop, nullptr, 0, nullptr);
}
