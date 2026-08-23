#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <Windows.h>

using namespace std;

class CUser
{
public:
	explicit CUser(int64_t _AccountID);
	~CUser();

	int64_t GetAccountID() const { return m_AccountID; }
	bool SetNicknameOnce(const string& _Nickname);
	string GetNickname();
	bool HasNickname();
	bool TryBeginNicknameRequest(uint64_t _RequestID);
	bool CompleteNicknameRequest(uint64_t _RequestID, const string& _Nickname);
	bool FinishDBRequest(uint64_t _RequestID);
	bool TryConsumeChatQuota();

private:
	int64_t m_AccountID;
	CRITICAL_SECTION m_UserLock;
	string m_Nickname;
	uint64_t m_PendingDBRequestID;
	chrono::steady_clock::time_point m_ChatWindowStart;
	uint32_t m_ChatMessagesInWindow;

	static constexpr uint32_t MAX_CHAT_MESSAGES_PER_SECOND = 5;
};
