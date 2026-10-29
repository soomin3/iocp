#include "TestSupport.h"
#include "LifetimeCatalog.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

int main(int ArgCount, char** Args)
{
	using namespace LifetimeVerification;
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	std::string Id;
	std::string Suite = "all";
	bool List = false;
	int Repeat = 1;
	for (int Index = 1; Index < ArgCount; ++Index)
	{
		const std::string_view Arg = Args[Index];
		if (Arg == "--list")
		{
			List = true;
		}
		else if (Arg == "--suite" && Index + 1 < ArgCount) Suite = Args[++Index];
		else if (Arg == "--case" && Index + 1 < ArgCount) Id = Args[++Index];
		else if (Arg == "--repeat" && Index + 1 < ArgCount)
		{
			const char* Value = Args[++Index];
			const auto Parsed = std::from_chars(Value, Value + std::strlen(Value), Repeat);
			if (Parsed.ec != std::errc{} || *Parsed.ptr != '\0' || Repeat < 1 || Repeat > 1000)
			{
				std::fprintf(stderr, "--repeat must be an integer from 1 through 1000.\n");
				return 2;
			}
		}
		else
		{
			std::fprintf(stderr, "Usage: LifetimeTests.exe (--case <id> [--repeat 1..1000] | --list) [--suite all|common|game]\n");
			return 2;
		}
	}
	if (Suite != "all" && Suite != "common" && Suite != "game")
	{
		std::fprintf(stderr, "--suite must be all, common, or game.\n");
		return 2;
	}
	if (List && !Id.empty())
	{
		std::fprintf(stderr, "--list and --case cannot be combined.\n");
		return 2;
	}
	const Scenario* Selected = nullptr;
	const std::wstring WideId(Id.begin(), Id.end());
	const std::wstring WideSuite(Suite.begin(), Suite.end());
	for (const auto& Scenario : kScenarios)
	{
		if (Suite != "all" && WideSuite != GroupId(Scenario.group)) continue;
		if (List) std::printf("%ls\n", Scenario.id);
		if (WideId == Scenario.id) Selected = &Scenario;
	}
	if (List) return 0;
	if (!Selected)
	{
		std::fprintf(stderr, "Unknown scenario. Run --list for available cases.\n");
		return 2;
	}

	WSADATA Wsa{};
	Check(WSAStartup(MAKEWORD(2, 2), &Wsa) == 0, "WSAStartup");
	std::atomic<ULONGLONG> Deadline{ GetTickCount64() + 20000 };
	std::jthread Watchdog([&](std::stop_token Stop)
	{
		while (!Stop.stop_requested())
		{
			if (GetTickCount64() > Deadline.load())
			{
				std::fprintf(stderr, "TIMEOUT: scenario iteration did not drain within 20 seconds.\n");
				ExitProcess(3);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	});
	const ULONGLONG Started = GetTickCount64();
	for (int Iteration = 1; Iteration <= Repeat; ++Iteration)
	{
		Deadline.store(GetTickCount64() + 20000);
		std::printf("RUN %s %d/%d\n", Id.c_str(), Iteration, Repeat);
		if (Selected->group == TestGroup::Game) RunGameServerCase(Id);
		else if (Id.starts_with("session_")) RunSessionLifetimeCase(Id);
		else RunDBLifetimeCase(Id);
		std::printf("PASS %s %d/%d\n", Id.c_str(), Iteration, Repeat);
	}
	Watchdog.request_stop();
	Watchdog.join();
	WSACleanup();
	std::printf("RESULT PASS group=%ls case=%s iterations=%d elapsed_ms=%llu\n",
		GroupId(Selected->group), Id.c_str(), Repeat, GetTickCount64() - Started);
	return 0;
}
