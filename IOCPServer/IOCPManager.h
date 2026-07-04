#pragma once
#include <WinSock2.h>
#include "Myinclude.h"
#include "Session.h"

using namespace std;

#define USER_SIZE 100

extern LPFN_ACCEPTEX g_AcceptEx;

class CIOCPManager
{
public:
	void Init();
	void Run();
	void Release();

private:
	void WorkerThread(HANDLE _iocpHandle);
	void InitAcceptEx(SOCKET _ListenSoceket);

	SOCKET m_ServerSocket;
	HANDLE m_IOCPHandle;
	vector<unique_ptr<CSession>> m_SessionPool;
};