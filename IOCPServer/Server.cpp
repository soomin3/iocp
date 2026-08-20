#include "pch.h"
#include "Server.h"

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

	m_PacketDispatcher.RegisterAllHandlers();

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
}
