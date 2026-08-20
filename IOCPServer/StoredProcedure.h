#pragma once
#include <sql.h>
#include <sqlext.h>

using namespace std;

class CStoredProcedure
{
public:
	virtual void Init();

private:
	SQLHSTMT m_StatementHandle;
	wstring m_Query;
};