#pragma once
#include <cstdint>
#include <string>

using namespace std;

struct SChatLogRecord
{
	uint64_t Sequence = 0;
	int64_t UtcMilliseconds = 0;
	int64_t AccountID = 0;
	string Nickname;
	string Message;
};
