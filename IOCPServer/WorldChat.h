#pragma once
#include <Windows.h>
#include <atomic>
#include <memory>
#include <queue>
#include <thread>
#include "ChatTypes.h"

using namespace std;

class CChatLogManager;
class CSession;
class CUser;
class CUserManager;

class CWorldChat
{
public:
	CWorldChat(CUserManager& _UserManager, CChatLogManager& _LogManager);
	~CWorldChat();

	bool Start();
	void Stop();

	bool Submit(const shared_ptr<CUser>& _User, CSession& _Session, uint64_t _Generation, const string& _Message);

	size_t GetAudienceCount();
	size_t GetQueueSize();
	uint64_t GetProcessedCount() const { return m_ProcessedCount.load(); }
	uint64_t GetRejectedCount() const { return m_RejectedCount.load(); }

private:
	void WorkerThread();
	void ProcessChat(SChatJob&& _Job);
	static int64_t GetUtcMilliseconds();

private:
	CRITICAL_SECTION m_ChatLock;
	CONDITION_VARIABLE m_ChatCondition;
	queue<SChatJob> m_ChatQueue;
	thread m_ChatThread;
	CUserManager& m_UserManager;
	CChatLogManager& m_LogManager;
	uint64_t m_NextSequence;
	bool m_StopRequested;
	bool m_Running;
	atomic_uint64_t m_ProcessedCount;
	atomic_uint64_t m_RejectedCount;

	static constexpr size_t MAX_CHAT_QUEUE_SIZE = 10000;
};
