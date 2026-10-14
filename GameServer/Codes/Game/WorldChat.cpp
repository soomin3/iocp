#include "pch.h"
#include "WorldChat.h"

#include "ChatLogManager.h"
#include "Packet.h"
#include "Session.h"
#include "User.h"
#include "UserManager.h"

CWorldChat::CWorldChat(CUserManager& _UserManager, CChatLogManager& _LogManager) :
	m_UserManager(_UserManager),
	m_LogManager(_LogManager),
	m_NextSequence(1),
	m_ProcessedCount(0),
	m_RejectedCount(0)
{
	InitializeCriticalSection(&m_ChatLock);
}

CWorldChat::~CWorldChat()
{
	DeleteCriticalSection(&m_ChatLock);
}

EWorldChatResult CWorldChat::Broadcast(const shared_ptr<CUser>& _User, CSession& _Session, uint64_t _Generation, const string& _Message)
{
	if (!_User || !_User->HasNickname() || !_Session.IsAuthenticated(_User, _Generation))
	{
		m_RejectedCount.fetch_add(1);
		return EWorldChatResult::REJECTED;
	}

	SActiveUserContext SenderContext;
	if (!m_UserManager.FindUser(_User->GetAccountID(), SenderContext) ||
		SenderContext.User != _User || SenderContext.Session != &_Session ||
		SenderContext.Generation != _Generation)
	{
		m_RejectedCount.fetch_add(1);
		return EWorldChatResult::REJECTED;
	}

	CCSLock Lock(&m_ChatLock);

	const vector<SActiveUserContext> ActiveUsers = m_UserManager.GetActiveUsers();
	bool SenderIncluded = false;

	for (const SActiveUserContext& UserContext : ActiveUsers)
	{
		if (UserContext.User == _User && UserContext.Session == &_Session &&
			UserContext.Generation == _Generation &&
			UserContext.Session->IsAuthenticated(UserContext.User, UserContext.Generation))
		{
			SenderIncluded = true;
			break;
		}
	}

	if (!SenderIncluded)
	{
		m_RejectedCount.fetch_add(1);
		return EWorldChatResult::REJECTED;
	}

	SChatLogRecord Record;
	Record.Sequence = m_NextSequence++;
	Record.UtcMilliseconds = GetUtcMilliseconds();
	Record.AccountID = _User->GetAccountID();
	Record.Nickname = _User->GetNickname();
	Record.Message = _Message;

	if (!m_LogManager.Enqueue(Record))
	{
		m_RejectedCount.fetch_add(1);
		return EWorldChatResult::LOG_FAILURE;
	}

	SC_CHAT_BROADCAST Packet;
	Packet.sequence = Record.Sequence;
	Packet.utcMilliseconds = Record.UtcMilliseconds;
	Packet.accountID = Record.AccountID;
	memcpy(Packet.nickname, Record.Nickname.data(), Record.Nickname.size());
	memcpy(Packet.message, Record.Message.data(), Record.Message.size());

	for (const SActiveUserContext& UserContext : ActiveUsers)
	{
		if (!UserContext.User || !UserContext.Session || !UserContext.User->HasNickname() ||
			!UserContext.Session->IsAuthenticated(UserContext.User, UserContext.Generation))
		{
			continue;
		}

		if (!UserContext.Session->Send(&Packet, sizeof(Packet), UserContext.Generation) &&
			UserContext.Session->GetGeneration() == UserContext.Generation)
		{
			UserContext.Session->Disconnect();
		}
	}

	m_ProcessedCount.fetch_add(1);
	return EWorldChatResult::SUCCESS;
}

size_t CWorldChat::GetAudienceCount()
{
	size_t AudienceCount = 0;
	const vector<SActiveUserContext> ActiveUsers = m_UserManager.GetActiveUsers();

	for (const SActiveUserContext& UserContext : ActiveUsers)
	{
		if (UserContext.User && UserContext.Session && UserContext.User->HasNickname() &&
			UserContext.Session->IsAuthenticated(UserContext.User, UserContext.Generation))
		{
			++AudienceCount;
		}
	}

	return AudienceCount;
}

int64_t CWorldChat::GetUtcMilliseconds()
{
	FILETIME FileTime{};
	GetSystemTimeAsFileTime(&FileTime);

	ULARGE_INTEGER Value{};
	Value.LowPart = FileTime.dwLowDateTime;
	Value.HighPart = FileTime.dwHighDateTime;

	constexpr uint64_t WINDOWS_TO_UNIX_EPOCH_100NS = 116444736000000000ULL;
	return static_cast<int64_t>((Value.QuadPart - WINDOWS_TO_UNIX_EPOCH_100NS) / 10000ULL);
}
