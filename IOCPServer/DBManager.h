#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
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

	template<typename PROCEDURE>
	bool ExecuteProcedureAsync(PROCEDURE _Procedure, function<void(typename PROCEDURE::ResultType)> _Callback)
	{
		if (!_Callback)
		{
			return false;
		}

		return Enqueue([this, Procedure = move(_Procedure), Callback = move(_Callback)]() mutable
			{
				typename PROCEDURE::ResultType Result = Procedure.Execute(m_ConnectionHandle);
				Callback(move(Result));
			});
	}

	bool AuthenticateAsync(string _ID, string _PW, ProcCallback _Callback);

private:
	void WorkerThread();
	void PrintDiagnostics(SQLSMALLINT _HandleType, SQLHANDLE _Handle, const wchar_t* _Operation) const;

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
