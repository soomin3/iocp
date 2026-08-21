#include "pch.h"
#include "DBManager.h"
#include "AuthProcedures.h"

CDBManager::CDBManager() : m_EnvironmentHandle(SQL_NULL_HENV), m_ConnectionHandle(SQL_NULL_HDBC), m_IsConnected(false), m_StopRequested(false)
{
	InitializeCriticalSection(&m_JobLock);
	InitializeConditionVariable(&m_JobCondition);
}

CDBManager::~CDBManager()
{
	StopWorker();
	Release();
	DeleteCriticalSection(&m_JobLock);
}

bool CDBManager::Init()
{
	SQLRETURN Result = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &m_EnvironmentHandle);

	if(!SQL_SUCCEEDED(Result))
	{
		return false;
	}

	Result = SQLSetEnvAttr(m_EnvironmentHandle, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

	if(!SQL_SUCCEEDED(Result))
	{
		Release();
		return false;
	}

	Result = SQLAllocHandle(SQL_HANDLE_DBC, m_EnvironmentHandle, &m_ConnectionHandle);
	if(!SQL_SUCCEEDED(Result))
	{
		Release();
		return false;
	}

	return true;
}

void CDBManager::Release()
{
	Disconnect();
	
	if (m_ConnectionHandle)
	{
		SQLFreeHandle(SQL_HANDLE_DBC, m_ConnectionHandle);
		m_ConnectionHandle = SQL_NULL_HDBC;
	}

	if (m_EnvironmentHandle)
	{
		SQLFreeHandle(SQL_HANDLE_ENV, m_EnvironmentHandle);
		m_EnvironmentHandle = SQL_NULL_HENV;
	}
}

bool CDBManager::Connect(const wstring& _ConnectionString)
{
	if (m_EnvironmentHandle == SQL_NULL_HENV || m_ConnectionHandle == SQL_NULL_HDBC)
	{
		return false;
	}

	SQLWCHAR OutConnectionString[1024] = {};
	SQLSMALLINT OutConnectionLen = 0;

	SQLRETURN Result = SQLDriverConnectW(m_ConnectionHandle, nullptr, (SQLWCHAR*)_ConnectionString.c_str(), SQL_NTS, OutConnectionString, 1024, &OutConnectionLen, SQL_DRIVER_NOPROMPT);
	if (!SQL_SUCCEEDED(Result))
	{
		return false;
	}

	m_IsConnected = true;
	return true;
}

void CDBManager::Disconnect()
{
	if (m_ConnectionHandle)
	{
		SQLDisconnect(m_ConnectionHandle);
		m_IsConnected = false;
	}
}

bool CDBManager::StartWorker()
{
	if (m_WorkerThread.joinable())
	{
		return false;
	}

	{
		CCSLock lock(&m_JobLock);
		m_StopRequested = false;
	}

	m_WorkerThread = thread(&CDBManager::WorkerThread, this);
	return true;
}

bool CDBManager::Enqueue(DBJob _Job)
{
	if (!_Job)
	{
		return false;
	}

	{
		CCSLock lock(&m_JobLock);

		if (m_StopRequested)
		{
			return false;
		}

		m_JobQueue.push(move(_Job));
		WakeConditionVariable(&m_JobCondition);
	}

	return true;
}

void CDBManager::StopWorker()
{
	{
		CCSLock lock(&m_JobLock);
		m_StopRequested = true;
		WakeAllConditionVariable(&m_JobCondition);
	}

	if (m_WorkerThread.joinable())
	{
		m_WorkerThread.join();
	}
}

void CDBManager::WorkerThread()
{
	while (true)
	{
		DBJob Job;
		{
			CCSLock lock(&m_JobLock);

			while(m_JobQueue.empty() && !m_StopRequested)
			{
				SleepConditionVariableCS(&m_JobCondition, &m_JobLock, INFINITE);
			}

			if (m_StopRequested && m_JobQueue.empty())
			{
				return;
			}

			Job = move(m_JobQueue.front());
			m_JobQueue.pop();
		}

		Job();
	}
}

bool CDBManager::AuthenticateAsync(string _ID, string _PW, ProcCallback _Callback)
{
	if (!_Callback)
	{
		return false;
	}

	return Enqueue([this, ID = move(_ID), PW = move(_PW), Callback = move(_Callback)]() mutable
		{
			SLoginProcedureResult Result = ExecuteLoginProcedure(ID, PW);

			Callback(move(Result));
		});
}

SLoginProcedureResult CDBManager::ExecuteLoginProcedure(string& _ID, string& _PW)
{
	CProcedure_UserLogin proc;

	return proc.Execute(m_ConnectionHandle, _ID, _PW);
}
