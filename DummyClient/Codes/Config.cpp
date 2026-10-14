#include "Config.h"
#include <Windows.h>
#include <charconv>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
	uint32_t ReadNumber(const std::string& _Name, const std::string& _Value, uint32_t _Min, uint32_t _Max)
	{
		uint32_t Value = 0;
		const auto Result = std::from_chars(_Value.data(), _Value.data() + _Value.size(), Value);
		if (Result.ec != std::errc{} || Result.ptr != _Value.data() + _Value.size() || Value < _Min || Value > _Max)
			throw std::runtime_error(_Name + " must be in [" + std::to_string(_Min) + ", " + std::to_string(_Max) + "]");
		return Value;
	}

	void ValidateAccount(const SAccount& _Account)
	{
		for (const std::string* Field : { &_Account.ID, &_Account.Password, &_Account.Nickname })
		{
			const size_t Limit = Field == &_Account.Nickname ? 64 : 32;
			if (Field->empty() || Field->size() >= Limit || Field->find('\0') != std::string::npos)
				throw std::runtime_error("Account fields must be nonempty; ID/password <=31 UTF-8 bytes, nickname <=63 bytes");
			wchar_t Wide[64]{};
			const int Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Field->data(), static_cast<int>(Field->size()), Wide, 64);
			if (Length == 0)
				throw std::runtime_error("Account fields must be valid UTF-8");
			if (Field == &_Account.Nickname)
			{
				int Points = 0;
				for (int Index = 0; Index < Length; ++Index)
				{
					if (iswcntrl(Wide[Index]))
						throw std::runtime_error("Nickname contains a control character");
					if (Wide[Index] < 0xDC00 || Wide[Index] > 0xDFFF)
						++Points;
				}
				if (Points > 16 || iswspace(Wide[0]) || iswspace(Wide[Length - 1]))
					throw std::runtime_error("Nickname must have <=16 code points and no surrounding whitespace");
			}
		}
	}
}

bool SConfig::IsProbe(size_t _Index) const
{
	return Scenario == "malformed" || (Scenario == "mixed" && _Index % 20 == 0);
}

bool SConfig::NeedsAccounts() const
{
	return Scenario != "connect-only" && Scenario != "malformed";
}

SConfig ParseConfig(int _Argc, char** _Argv)
{
	SConfig Config;
	std::vector<std::pair<std::string, std::string>> Options;
	std::set<std::string> Seen;
	for (int Index = 1; Index < _Argc; ++Index)
	{
		const std::string Key = _Argv[Index];
		if (Key == "--help")
		{
			Config.Help = true;
			return Config;
		}
		if (!Key.starts_with("--") || Index + 1 == _Argc || std::string_view(_Argv[Index + 1]).starts_with("--"))
			throw std::runtime_error("Expected --option value; use --help");
		if (!Seen.insert(Key).second)
			throw std::runtime_error("Duplicate option: " + Key);
		Options.emplace_back(Key, _Argv[++Index]);
	}
	for (const auto& [Key, Value] : Options)
	{
		if (Key != "--preset") continue;
		if (Value == "lineage" || Value == "stress")
		{
			Config.Clients = Value == "lineage" ? 5000 : 10000;
			Config.ConnectRate = Value == "lineage" ? 250 : 500;
			Config.DurationSeconds = Value == "lineage" ? 120 : 180;
			Config.IntervalMs = Value == "lineage" ? 5000 : 1000;
			Config.ActivePercent = 1;
		}
		else if (Value != "smoke")
			throw std::runtime_error("Unknown preset: " + Value);
	}
	std::string AccountsPath;
	std::string Prefix = "dummy_";
	std::string NicknamePrefix = "dc_";
	std::string PasswordEnv = "DUMMY_PASSWORD";
	for (const auto& [Key, Value] : Options)
	{
		if (Key == "--preset") continue;
		if (Key == "--host") Config.Host = Value;
		else if (Key == "--local-ip") Config.LocalIP = Value;
		else if (Key == "--scenario") Config.Scenario = Value;
		else if (Key == "--csv") Config.CsvPath = Value;
		else if (Key == "--accounts") AccountsPath = Value;
		else if (Key == "--id-prefix") Prefix = Value;
		else if (Key == "--nickname-prefix") NicknamePrefix = Value;
		else if (Key == "--password-env") PasswordEnv = Value;
		else if (Key == "--port") Config.Port = ReadNumber(Key, Value, 1, 65535);
		else if (Key == "--clients") Config.Clients = ReadNumber(Key, Value, 1, 100000);
		else if (Key == "--duration") Config.DurationSeconds = ReadNumber(Key, Value, 1, 86400);
		else if (Key == "--connect-rate") Config.ConnectRate = ReadNumber(Key, Value, 0, 100000);
		else if (Key == "--interval-ms") Config.IntervalMs = ReadNumber(Key, Value, 1, 600000);
		else if (Key == "--active-percent") Config.ActivePercent = ReadNumber(Key, Value, 0, 100);
		else if (Key == "--timeout-ms") Config.TimeoutMs = ReadNumber(Key, Value, 100, 600000);
		else if (Key == "--reconnect-delay-ms") Config.ReconnectDelayMs = ReadNumber(Key, Value, 1, 600000);
		else if (Key == "--fault-every-ms") Config.FaultEveryMs = ReadNumber(Key, Value, 100, 3600000);
		else if (Key == "--disconnect-percent") Config.DisconnectPercent = ReadNumber(Key, Value, 1, 100);
		else if (Key == "--churn-rate") Config.ChurnRate = ReadNumber(Key, Value, 1, 100000);
		else if (Key == "--report-ms") Config.ReportMs = ReadNumber(Key, Value, 100, 60000);
		else if (Key == "--start-index") Config.StartIndex = ReadNumber(Key, Value, 0, 1000000000);
		else throw std::runtime_error("Unknown option: " + Key);
	}
	const std::set<std::string> Scenarios{ "steady", "connect-only", "reconnect", "churn", "malformed", "mixed" };
	if (!Scenarios.contains(Config.Scenario))
		throw std::runtime_error("Unknown scenario: " + Config.Scenario);
	if (!Config.NeedsAccounts()) return Config;
	if (!AccountsPath.empty())
	{
		std::ifstream Input(AccountsPath, std::ios::binary);
		if (!Input) throw std::runtime_error("Cannot open accounts file");
		std::string Line;
		std::set<std::string> IDs;
		std::set<std::string> Nicknames;
		while (std::getline(Input, Line))
		{
			if (Line.starts_with("\xEF\xBB\xBF")) Line.erase(0, 3);
			if (!Line.empty() && Line.back() == '\r') Line.pop_back();
			if (Line.empty() || Line[0] == '#') continue;
			const size_t First = Line.find('\t');
			const size_t Second = First == std::string::npos ? First : Line.find('\t', First + 1);
			if (Second == std::string::npos || Line.find('\t', Second + 1) != std::string::npos)
				throw std::runtime_error("Accounts file needs exactly three tab-separated fields: ID, password, nickname");
			SAccount Account{ Line.substr(0, First), Line.substr(First + 1, Second - First - 1), Line.substr(Second + 1) };
			ValidateAccount(Account);
			if (!IDs.insert(Account.ID).second || !Nicknames.insert(Account.Nickname).second)
				throw std::runtime_error("Duplicate ID or nickname in accounts file");
			Config.Accounts.push_back(std::move(Account));
		}
		if (Config.Accounts.size() < Config.Clients)
			throw std::runtime_error("Accounts file has fewer rows than --clients");
		Config.Accounts.resize(Config.Clients);
	}
	else
	{
		char* RawPassword = nullptr;
		size_t Length = 0;
		if (_dupenv_s(&RawPassword, &Length, PasswordEnv.c_str()) != 0 || RawPassword == nullptr)
			throw std::runtime_error("Set " + PasswordEnv + " or supply --accounts (existing test accounts are required)");
		const std::string Password(RawPassword);
		free(RawPassword);
		for (uint32_t Index = 0; Index < Config.Clients; ++Index)
		{
			const std::string Suffix = std::to_string(static_cast<uint64_t>(Config.StartIndex) + Index);
			SAccount Account{ Prefix + Suffix, Password, NicknamePrefix + Suffix };
			ValidateAccount(Account);
			Config.Accounts.push_back(std::move(Account));
		}
	}
	return Config;
}

void PrintUsage()
{
	std::cout <<
		"DummyClient: IOCP load / fault runner for GameServer\n"
		"  --preset smoke|lineage|stress  (100 / 5000 / 10000 clients; explicit options override)\n"
		"  --scenario steady|connect-only|reconnect|churn|malformed|mixed\n"
		"  --host 127.0.0.1 --port 8694 --local-ip 0.0.0.0\n"
		"  --clients 100 --duration 60 --connect-rate 100 (attempts/sec, 0=burst)\n"
		"  --accounts accounts.local.tsv (UTF-8 ID<TAB>password<TAB>nickname, no header)\n"
		"  Or: --id-prefix dummy_ --nickname-prefix dc_ --start-index 1\n"
		"      --password-env DUMMY_PASSWORD (environment variable, no automatic account creation)\n"
		"  --interval-ms 1000 --active-percent 100 --timeout-ms 5000\n"
		"  --reconnect-delay-ms 1000 --fault-every-ms 10000 --disconnect-percent 25\n"
		"  --churn-rate 10 --report-ms 1000 --csv DummyClient/results/run.csv\n"
		"Duration includes connection ramp. Ctrl+C stops and drains outstanding I/O.\n"
		"Latency: monotonic request-to-response RTT; chat matches own echoed message.\n"
		"GameServer currently has a 100-session pool; adjust it before large-scale runs.\n"
		"The lineage preset is a configurable test target, not an official Lineage capacity.\n";
}
