#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

using namespace std;

class CChatLogManager;
class CSession;
class CUser;
class CUserManager;

enum class EWorldChatResult : uint8_t
{
	SUCCESS,
	REJECTED,
	LOG_FAILURE
};

class CWorldChat
{
public:
	CWorldChat(CUserManager& _UserManager, CChatLogManager& _LogManager);
	~CWorldChat();

	EWorldChatResult Broadcast(const shared_ptr<CUser>& _User, CSession& _Session, uint64_t _Generation, const string& _Message);

	size_t GetAudienceCount();
	uint64_t GetProcessedCount() const { return m_ProcessedCount.load(); }
	uint64_t GetRejectedCount() const { return m_RejectedCount.load(); }

private:
	static int64_t GetUtcMilliseconds();

private:
	CRITICAL_SECTION m_ChatLock;
	CUserManager& m_UserManager;
	CChatLogManager& m_LogManager;
	uint64_t m_NextSequence;
	atomic_uint64_t m_ProcessedCount;
	atomic_uint64_t m_RejectedCount;
};
