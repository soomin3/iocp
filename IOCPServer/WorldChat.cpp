#include "pch.h"
#include "WorldChat.h"

#include <system_error>

#include "ChatLogManager.h"
#include "Packet.h"
#include "Session.h"
#include "User.h"
#include "UserManager.h"

CWorldChat::CWorldChat(CUserManager& _UserManager, CChatLogManager& _LogManager) :
	m_UserManager(_UserManager),
	m_LogManager(_LogManager),
	m_NextSequence(1),
	m_StopRequested(true),
	m_Running(false),
	m_ProcessedCount(0),
	m_RejectedCount(0)
{
	InitializeCriticalSection(&m_ChatLock);
	InitializeConditionVariable(&m_ChatCondition);
}

CWorldChat::~CWorldChat()
{
	Stop();
	DeleteCriticalSection(&m_ChatLock);
}

bool CWorldChat::Start()
{
	{
		CCSLock Lock(&m_ChatLock);
		if (m_Running)
		{
			return false;
		}

		m_StopRequested = false;
		m_Running = true;
	}

	try
	{
		m_ChatThread = thread(&CWorldChat::WorkerThread, this);
	}
	catch (const system_error& Error)
	{
		CCSLock Lock(&m_ChatLock);
		m_StopRequested = true;
		m_Running = false;
		cout << "World chat worker start failed: " << Error.what() << '\n';
		return false;
	}

	return true;
}

void CWorldChat::Stop()
{
	{
		CCSLock Lock(&m_ChatLock);
		m_StopRequested = true;
		WakeAllConditionVariable(&m_ChatCondition);
	}

	if (m_ChatThread.joinable())
	{
		m_ChatThread.join();
	}

	CCSLock Lock(&m_ChatLock);
	m_Running = false;
}

bool CWorldChat::Submit(const shared_ptr<CUser>& _User, CSession& _Session, uint64_t _Generation, const string& _Message)
{
	if (!_User || !_User->HasNickname() || !_Session.IsAuthenticated(_User, _Generation))
	{
		m_RejectedCount.fetch_add(1);
		return false;
	}

	SActiveUserContext SenderContext;
	if (!m_UserManager.FindUser(_User->GetAccountID(), SenderContext) ||
		SenderContext.User != _User || SenderContext.Session != &_Session ||
		SenderContext.Generation != _Generation)
	{
		m_RejectedCount.fetch_add(1);
		return false;
	}

	vector<SActiveUserContext> ActiveUsers = m_UserManager.GetActiveUsers();
	SChatJob Job;
	Job.Record.UtcMilliseconds = GetUtcMilliseconds();
	Job.Record.AccountID = _User->GetAccountID();
	Job.Record.Nickname = _User->GetNickname();
	Job.Record.Message = _Message;
	Job.Sender = &_Session;
	Job.SenderGeneration = _Generation;
	Job.Recipients.reserve(ActiveUsers.size());
	bool SenderIncluded = false;

	for (const SActiveUserContext& UserContext : ActiveUsers)
	{
		if (UserContext.User && UserContext.Session && UserContext.User->HasNickname() &&
			UserContext.Session->IsAuthenticated(UserContext.User, UserContext.Generation))
		{
			Job.Recipients.push_back({ UserContext.Session, UserContext.Generation });
			SenderIncluded = SenderIncluded ||
				(UserContext.User == _User && UserContext.Session == &_Session &&
					UserContext.Generation == _Generation);
		}
	}

	if (!SenderIncluded)
	{
		m_RejectedCount.fetch_add(1);
		return false;
	}

	{
		CCSLock Lock(&m_ChatLock);

		if (!m_Running || m_StopRequested || m_ChatQueue.size() >= MAX_CHAT_QUEUE_SIZE)
		{
			m_RejectedCount.fetch_add(1);
			return false;
		}

		Job.Record.Sequence = m_NextSequence++;
		m_ChatQueue.push(move(Job));
		WakeConditionVariable(&m_ChatCondition);
	}

	return true;
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

size_t CWorldChat::GetQueueSize()
{
	CCSLock Lock(&m_ChatLock);
	return m_ChatQueue.size();
}

void CWorldChat::WorkerThread()
{
	while (true)
	{
		SChatJob Job;

		{
			CCSLock Lock(&m_ChatLock);

			while (m_ChatQueue.empty() && !m_StopRequested)
			{
				SleepConditionVariableCS(&m_ChatCondition, &m_ChatLock, INFINITE);
			}

			if (m_StopRequested && m_ChatQueue.empty())
			{
				return;
			}

			Job = move(m_ChatQueue.front());
			m_ChatQueue.pop();
		}

		ProcessChat(move(Job));
	}
}

void CWorldChat::ProcessChat(SChatJob&& _Job)
{
	if (!m_LogManager.Enqueue(_Job.Record))
	{
		m_RejectedCount.fetch_add(1);
		SC_CHAT_ERROR ErrorPacket;
		ErrorPacket.error = EChatError::LOG_FAILURE;
		if (!_Job.Sender->Send(&ErrorPacket, sizeof(ErrorPacket), _Job.SenderGeneration) &&
			_Job.Sender->GetGeneration() == _Job.SenderGeneration)
		{
			_Job.Sender->Disconnect();
		}
		return;
	}

	SC_CHAT_BROADCAST Packet;
	Packet.sequence = _Job.Record.Sequence;
	Packet.utcMilliseconds = _Job.Record.UtcMilliseconds;
	Packet.accountID = _Job.Record.AccountID;
	memcpy(Packet.nickname, _Job.Record.Nickname.data(), _Job.Record.Nickname.size());
	memcpy(Packet.message, _Job.Record.Message.data(), _Job.Record.Message.size());

	for (const SChatRecipient& Recipient : _Job.Recipients)
	{
		if (!Recipient.Session->Send(&Packet, sizeof(Packet), Recipient.Generation) &&
			Recipient.Session->GetGeneration() == Recipient.Generation)
		{
			Recipient.Session->Disconnect();
		}
	}

	m_ProcessedCount.fetch_add(1);
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
