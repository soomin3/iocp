#pragma once

#include "DBManager.h"
#include "IOCPManager.h"
#include "PacketDispatcher.h"

using namespace std;

class CServer
{
public:
	CServer();

	bool Init();
	void Run();

private:
	CDBManager m_DBManager;
	CPacketDispatcher m_PacketDispatcher;
	CIOCPManager m_IOCPManager;
};