#pragma once
#include "Session.h"
#include <vector>

using namespace std;

#define USER_SIZE 100

extern LPFN_ACCEPTEX g_AcceptEx;

class CIOCPManager
{
public:
	explicit CIOCPManager(CPacketDispatcher& _PacketDispatcher);
	~CIOCPManager();

	bool Init();
	void Run();
	void Release();

private:
	void WorkerThread(HANDLE _iocpHandle);
	void InitAcceptEx(SOCKET _ListenSoceket);

	void DisconnectSession(CSession* _Session);
	void StopWorkerThreads();

	SOCKET m_ServerSocket;
	HANDLE m_IOCPHandle;
	vector<shared_ptr<CSession>> m_SessionPool;
	CPacketDispatcher& m_PacketDispatcher;

	atomic_bool m_IsRunning;
	vector<thread> m_WorkerThreads;
	//CUserManager m_UserManager;
};