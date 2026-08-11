#pragma once
#include "Session.h"
#include <vector>

class CPacketManager;

using namespace std;

#define USER_SIZE 100

extern LPFN_ACCEPTEX g_AcceptEx;

class CIOCPManager
{
public:
	CIOCPManager();
	~CIOCPManager();

	void Init();
	void Run();
	void Release();

private:
	void WorkerThread(HANDLE _iocpHandle);
	void InitAcceptEx(SOCKET _ListenSoceket);

	void DisconnectSession(CSession* _Session);

	SOCKET m_ServerSocket;
	HANDLE m_IOCPHandle;
	vector<shared_ptr<CSession>> m_SessionPool;
	unique_ptr<CPacketManager> m_PacketManager;
	//CUserManager m_UserManager;
};