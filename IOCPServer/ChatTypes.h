#pragma once
#include <cstdint>
#include <string>
#include <vector>

using namespace std;

class CSession;

struct SChatLogRecord
{
	uint64_t Sequence = 0;
	int64_t UtcMilliseconds = 0;
	int64_t AccountID = 0;
	string Nickname;
	string Message;
};

struct SChatRecipient
{
	CSession* Session = nullptr;
	uint64_t Generation = 0;
};

struct SChatJob
{
	SChatLogRecord Record;
	CSession* Sender = nullptr;
	uint64_t SenderGeneration = 0;
	vector<SChatRecipient> Recipients;
};
