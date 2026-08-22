#include "pch.h"
#include "User.h"

CUser::CUser(int64_t _AccountID) :
	m_AccountID(_AccountID),
	m_PendingDBRequestID(0),
	m_ChatWindowStart(chrono::steady_clock::now()),
	m_ChatMessagesInWindow(0)
{
	InitializeCriticalSection(&m_UserLock);
}

CUser::~CUser()
{
	DeleteCriticalSection(&m_UserLock);
}

bool CUser::SetNicknameOnce(const string& _Nickname)
{
	if (_Nickname.empty())
	{
		return false;
	}

	CCSLock Lock(&m_UserLock);

	if (!m_Nickname.empty() || m_PendingDBRequestID != 0)
	{
		return false;
	}

	m_Nickname = _Nickname;
	return true;
}

string CUser::GetNickname()
{
	CCSLock Lock(&m_UserLock);
	return m_Nickname;
}

bool CUser::HasNickname()
{
	CCSLock Lock(&m_UserLock);
	return !m_Nickname.empty();
}

bool CUser::TryBeginNicknameRequest(uint64_t _RequestID)
{
	if (_RequestID == 0)
	{
		return false;
	}

	CCSLock Lock(&m_UserLock);
	if (m_PendingDBRequestID != 0 || !m_Nickname.empty())
	{
		return false;
	}

	m_PendingDBRequestID = _RequestID;
	return true;
}

bool CUser::CompleteNicknameRequest(uint64_t _RequestID, const string& _Nickname)
{
	CCSLock Lock(&m_UserLock);
	if (_RequestID == 0 || m_PendingDBRequestID != _RequestID)
	{
		return false;
	}

	m_PendingDBRequestID = 0;
	if (_Nickname.empty() || !m_Nickname.empty())
	{
		return false;
	}

	m_Nickname = _Nickname;
	return true;
}

bool CUser::FinishDBRequest(uint64_t _RequestID)
{
	CCSLock Lock(&m_UserLock);
	if (_RequestID == 0 || m_PendingDBRequestID != _RequestID)
	{
		return false;
	}

	m_PendingDBRequestID = 0;
	return true;
}

bool CUser::TryConsumeChatQuota()
{
	CCSLock Lock(&m_UserLock);

	const auto Now = chrono::steady_clock::now();
	if (Now - m_ChatWindowStart >= chrono::seconds(1))
	{
		m_ChatWindowStart = Now;
		m_ChatMessagesInWindow = 0;
	}

	if (m_ChatMessagesInWindow >= MAX_CHAT_MESSAGES_PER_SECOND)
	{
		return false;
	}

	++m_ChatMessagesInWindow;
	return true;
}
