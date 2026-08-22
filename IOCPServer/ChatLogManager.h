#pragma once
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <queue>
#include <string>
#include <thread>
#include "ChatTypes.h"

using namespace std;

class CChatLogManager
{
public:
	CChatLogManager();
	~CChatLogManager();

	bool Start(const filesystem::path& _Directory);
	void Stop();
	bool Enqueue(SChatLogRecord _Record);

	size_t GetQueueSize();
	uint64_t GetWrittenCount() const { return m_WrittenCount.load(); }
	uint64_t GetFailureCount() const { return m_FailureCount.load(); }

private:
	void WorkerThread();
	bool OpenLogFile(ofstream& _Stream, string& _OpenedDate);
	bool WriteRecord(ofstream& _Stream, const SChatLogRecord& _Record);
	static string GetUtcDate();
	static string EscapeJson(const string& _Value);

private:
	CRITICAL_SECTION m_LogLock;
	CONDITION_VARIABLE m_LogCondition;
	queue<SChatLogRecord> m_LogQueue;
	thread m_LogThread;
	filesystem::path m_LogDirectory;
	bool m_StopRequested;
	bool m_Running;
	bool m_WriteHealthy;
	atomic_uint64_t m_WrittenCount;
	atomic_uint64_t m_FailureCount;

	static constexpr size_t MAX_LOG_QUEUE_SIZE = 10000;
};
