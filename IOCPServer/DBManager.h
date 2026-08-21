#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <sql.h>
#include <sqlext.h>
#include "AuthProcedureTypes.h"

using namespace std;
using DBJob = function<void()>;
using ProcCallback = function<void(SLoginProcedureResult)>;

class CDBManager
{
public:
	CDBManager();
	~CDBManager();

	bool Init();
	void Release();

	bool Connect(const wstring& _ConnectionString);
	void Disconnect();

	bool IsConnected() const { return m_IsConnected; }

	bool StartWorker();
	bool Enqueue(DBJob _Job);
	void StopWorker();

	bool AuthenticateAsync(string _ID, string _PW, ProcCallback _Callback);

private:
	void WorkerThread();

	SLoginProcedureResult ExecuteLoginProcedure(string& _ID, string& _PW);

private:
	SQLHENV m_EnvironmentHandle;
	SQLHDBC m_ConnectionHandle;

	bool m_IsConnected;

	CRITICAL_SECTION m_JobLock;
	CONDITION_VARIABLE m_JobCondition;
	queue<DBJob> m_JobQueue;

	thread m_WorkerThread;
	bool m_StopRequested;
};
