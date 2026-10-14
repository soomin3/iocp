#include "pch.h"
#include "Server.h"

CServer::CServer()
	: m_WorldChat(m_UserManager, m_ChatLogManager),
	m_AuthHandlers(m_DBClient, m_UserManager),
	m_ChatHandlers(m_WorldChat),
	m_IOCPManager(m_PacketDispatcher, m_UserManager)
{
}

CServer::~CServer()
{
	m_DBClient.StopWorker();
}

bool CServer::Init()
{
	if (!m_ChatLogManager.Start("logs"))
	{
		cout << "Chat Log Manager Start Failed" << endl;
		return false;
	}
	cout << "Chat Log Manager Start Success" << endl;

	if (!m_AuthHandlers.RegisterAll(m_PacketDispatcher))
	{
		cout << "Auth Handler Register Failed" << endl;
		return false;
	}

	cout << "Auth Handler Register Success" << endl;

	if (!m_ChatHandlers.RegisterAll(m_PacketDispatcher))
	{
		cout << "Chat Handler Register Failed" << endl;
		return false;
	}
	cout << "Chat Handler Register Success" << endl;

	if (!m_IOCPManager.Init())
	{
		cout << "IOCP Manager Init Failed" << endl;
		return false;
	}
	cout << "IOCP Manager Init Success" << endl;

	if(!m_DBClient.StartWorker(m_IOCPManager))
	{
		cout << "DBServer connection worker Failed" << endl;
		return false;
	}
	cout << "DBServer connection worker Success" << endl;

	return true;
}

void CServer::Run()
{
	if (m_IOCPManager.Run())
	{
		thread ConsoleThread(&CServer::ProcessConsoleCommand, this);
		ConsoleThread.join();
	}

	m_DBClient.StopWorker();
	m_IOCPManager.Stop();
	m_ChatLogManager.Stop();
	m_IOCPManager.Release();
}

void CServer::ProcessConsoleCommand()
{
	cout << "Commands : help, quit, status\n";

	string Command;

	while (getline(cin, Command))
	{
		if (Command == "help")
		{
			cout << "help - Show commands\n"
				<< "quit - Stop server\n"
				<< "status - Show server status\n";
		}
		else if (Command == "quit")
		{
			return;
		}
		else if (Command == "status")
		{
			m_IOCPManager.PrintStatus();
			cout << "dbQueue=" << m_DBClient.GetPendingJobCount()
				<< ", activeUsers=" << m_UserManager.GetUserCount()
				<< ", worldChatAudience=" << m_WorldChat.GetAudienceCount()
				<< ", worldChatProcessed=" << m_WorldChat.GetProcessedCount()
				<< ", worldChatRejected=" << m_WorldChat.GetRejectedCount()
				<< ", logQueue=" << m_ChatLogManager.GetQueueSize()
				<< ", logWritten=" << m_ChatLogManager.GetWrittenCount()
				<< ", logFailures=" << m_ChatLogManager.GetFailureCount()
				<< '\n';
		}
		else if (!Command.empty())
		{
			cout << "Unknown command: " << Command << '\n';
		}
	}
}
