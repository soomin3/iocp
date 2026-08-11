#include "pch.h"
#include "DBManager.h"

CDBManager::CDBManager() : m_EnvironmentHandle(SQL_NULL_HENV), m_ConnectionHandle(SQL_NULL_HDBC), m_IsConnected(false)
{
}

CDBManager::~CDBManager()
{
	Release();
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
