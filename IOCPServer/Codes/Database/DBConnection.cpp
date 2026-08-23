#include "pch.h"
#include "DBConnection.h"

CDBConnection::CDBConnection(): m_EnvironmentHandle(SQL_NULL_HENV), m_ConnectionHandle(SQL_NULL_HDBC), m_IsConnected(false)
{
}

CDBConnection::~CDBConnection()
{
	Release();
}

bool CDBConnection::Init()
{
	m_LastError = {};

	if (m_EnvironmentHandle != SQL_NULL_HENV || m_ConnectionHandle != SQL_NULL_HDBC)
	{
		m_LastError = MakeDBError(EDBErrorCode::InvalidConnection, L"CDBConnection::Init", L"The DB connection is already initialized");
		return false;
	}

	SQLRETURN Result = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &m_EnvironmentHandle);
	if (!SQL_SUCCEEDED(Result))
	{
		m_LastError = MakeDBError(EDBErrorCode::InvalidConnection, L"SQLAllocHandle(ENV)", L"Failed to allocate the ODBC environment handle");
		return false;
	}

	Result = SQLSetEnvAttr(m_EnvironmentHandle, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

	if (!SQL_SUCCEEDED(Result))
	{
		m_LastError = ReadOdbcError(EDBErrorCode::InvalidConnection, SQL_HANDLE_ENV, m_EnvironmentHandle, L"SQLSetEnvAttr");
		Release();
		return false;
	}

	Result = SQLAllocHandle(SQL_HANDLE_DBC, m_EnvironmentHandle, &m_ConnectionHandle);
	if (!SQL_SUCCEEDED(Result))
	{
		m_LastError = ReadOdbcError(EDBErrorCode::InvalidConnection, SQL_HANDLE_ENV, m_EnvironmentHandle, L"SQLAllocHandle(DBC)");
		Release();
		return false;
	}

	return true;
}

void CDBConnection::Release()
{
	Disconnect();

	if (m_ConnectionHandle != SQL_NULL_HDBC)
	{
		SQLFreeHandle(SQL_HANDLE_DBC, m_ConnectionHandle);
		m_ConnectionHandle = SQL_NULL_HDBC;
	}

	if (m_EnvironmentHandle != SQL_NULL_HENV)
	{
		SQLFreeHandle(SQL_HANDLE_ENV, m_EnvironmentHandle);
		m_EnvironmentHandle = SQL_NULL_HENV;
	}
}

bool CDBConnection::Connect(const wstring& _ConnectionString)
{
	m_LastError = {};

	if (m_EnvironmentHandle == SQL_NULL_HENV || m_ConnectionHandle == SQL_NULL_HDBC || m_IsConnected || _ConnectionString.empty())
	{
		m_LastError = MakeDBError(EDBErrorCode::InvalidConnection, L"CDBConnection::Connect", L"The connection is not initialized, is already connected, or has an empty connection string");
		return false;
	}

	SQLWCHAR OutConnectionString[1024] = {};
	SQLSMALLINT OutConnectionLength = 0;
	constexpr SQLULEN LOGIN_TIMEOUT_SECONDS = 5;

	const SQLRETURN TimeoutResult = SQLSetConnectAttr(m_ConnectionHandle, SQL_ATTR_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(LOGIN_TIMEOUT_SECONDS), 0);
	if (!SQL_SUCCEEDED(TimeoutResult))
	{
		m_LastError = ReadOdbcError(EDBErrorCode::InvalidConnection, SQL_HANDLE_DBC, m_ConnectionHandle, L"SQLSetConnectAttr(SQL_ATTR_LOGIN_TIMEOUT)");
		return false;
	}

	const SQLRETURN Result = SQLDriverConnectW(m_ConnectionHandle, nullptr, (SQLWCHAR*)_ConnectionString.c_str(), SQL_NTS, OutConnectionString, static_cast<SQLSMALLINT>(size(OutConnectionString)), &OutConnectionLength, SQL_DRIVER_NOPROMPT);

	if (!SQL_SUCCEEDED(Result))
	{
		m_LastError = ReadOdbcError(EDBErrorCode::InvalidConnection, SQL_HANDLE_DBC, m_ConnectionHandle, L"SQLDriverConnectW");
		return false;
	}

	m_IsConnected = true;
	return true;
}

void CDBConnection::Disconnect()
{
	if (m_ConnectionHandle != SQL_NULL_HDBC && m_IsConnected)
	{
		SQLDisconnect(m_ConnectionHandle);
		m_IsConnected = false;
	}
}
