#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SAccount
{
	std::string ID;
	std::string Password;
	std::string Nickname;
};

struct SConfig
{
	std::string Host = "127.0.0.1";
	std::string LocalIP = "0.0.0.0";
	std::string Scenario = "steady";
	std::string CsvPath;
	uint32_t Port = 8694;
	uint32_t Clients = 100;
	uint32_t DurationSeconds = 60;
	uint32_t ConnectRate = 100;
	uint32_t IntervalMs = 1000;
	uint32_t ActivePercent = 100;
	uint32_t TimeoutMs = 5000;
	uint32_t ReconnectDelayMs = 1000;
	uint32_t FaultEveryMs = 10000;
	uint32_t DisconnectPercent = 25;
	uint32_t ChurnRate = 10;
	uint32_t ReportMs = 1000;
	uint32_t StartIndex = 1;
	bool Help = false;
	std::vector<SAccount> Accounts;

	bool IsProbe(size_t _Index) const;
	bool NeedsAccounts() const;
};

SConfig ParseConfig(int _Argc, char** _Argv);
void PrintUsage();
