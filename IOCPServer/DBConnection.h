#pragma once
#include <sql.h>
#include <sqlext.h>
#include <string>
#include "DBError.h"

using namespace std;

class CDBConnection
{
public:
	CDBConnection();
	~CDBConnection();

	CDBConnection(const CDBConnection&) = delete;
	CDBConnection& operator=(const CDBConnection&) = delete;

	bool Init();
	void Release();

	bool Connect(const wstring& _ConnectionString);
	void Disconnect();

	bool IsConnected() const { return m_IsConnected; }
	SQLHDBC GetHandle() const { return m_ConnectionHandle; }
	const SDBError& GetLastError() const { return m_LastError; }

private:
	SQLHENV m_EnvironmentHandle;
	SQLHDBC m_ConnectionHandle;
	bool m_IsConnected;
	SDBError m_LastError;
};

