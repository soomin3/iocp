#include "pch.h"
#include "Server.h"
#include "AuthHandlers.h"

CServer::CServer() : m_IOCPManager(m_PacketDispatcher)
{
}

bool CServer::Init()
{
	if (!m_DBManager.Init())
	{
		cout << "SQL Server Init Failed" << endl;
		return false;
	}
	cout << "SQL Server Init Success" << endl;

	//Result = DBManager.Connect(L"Driver={SQL Server};Server=127.0.0.1;Database=TestDB;Trusted_Connection=yes;");
	wstring  ConnectionString = L"Driver={SQL Server};Server=127.0.0.1,1433;Database=TestDB;UID = sa;PWD = test;TrustedServerCertificate=yes;";

	if (!m_DBManager.Connect(ConnectionString))
	{
		cout << "SQL Server Connect Failed" << endl;
		return false;
	}
	cout << "SQL Server Connect Success" << endl;

	if(!m_DBManager.StartWorker())
	{
		cout << "SQL Server Start Worker Failed" << endl;
		return false;
	}
	cout << "SQL Server Start Worker Success" << endl;

	if (!RegisterAuthHandlers(m_PacketDispatcher, m_DBManager))
	{
		cout << "Auth Handler Register Failed" << endl;
		return false;
	}

	cout << "Auth Handler Register Success" << endl;

	if (!m_IOCPManager.Init())
	{
		cout << "IOCP Manager Init Failed" << endl;
		return false;
	}
	cout << "IOCP Manager Init Success" << endl;

	return true;
}

void CServer::Run()
{
	m_IOCPManager.Run();

	thread ConsoleThread(&CServer::ProcessConsoleCommand, this);
	ConsoleThread.join();

	m_IOCPManager.Stop();
	m_DBManager.StopWorker();
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
		}
		else if (!Command.empty())
		{
			cout << "Unknown command: " << Command << '\n';
		}
	}
}
