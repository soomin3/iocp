#pragma once
#include <sql.h>
#include <sqlext.h>

using namespace std;

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
	//bool AuthenticateUser(const string& id, const string& pw);

private:
	SQLHENV m_EnvironmentHandle;
	SQLHDBC m_ConnectionHandle;

	bool m_IsConnected;
};
