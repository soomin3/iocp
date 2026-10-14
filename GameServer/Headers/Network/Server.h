#pragma once
#include "AuthHandlers.h"
#include "ChatHandlers.h"
#include "ChatLogManager.h"
#include "DBClient.h"
#include "IOCPManager.h"
#include "PacketDispatcher.h"
#include "UserManager.h"
#include "WorldChat.h"

using namespace std;

class CServer
{
public:
	CServer();
	~CServer();

	bool Init();
	void Run();

	void ProcessConsoleCommand();

private:
	CDBClient m_DBClient;
	CUserManager m_UserManager;
	CChatLogManager m_ChatLogManager;
	CWorldChat m_WorldChat;
	CAuthHandlers m_AuthHandlers;
	CChatHandlers m_ChatHandlers;
	CPacketDispatcher m_PacketDispatcher;
	CIOCPManager m_IOCPManager;
};
