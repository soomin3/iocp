#pragma once
#include <sql.h>
#include <sqlext.h>
#include <string>
#include "DBError.h"

using namespace std;

class CDBStatement
{
public:
	explicit CDBStatement(SQLHDBC _ConnectionHandle);
	~CDBStatement();

	bool IsValid() const { return m_StatementHandle != SQL_NULL_HSTMT; }

	bool BindParameter(
		SQLUSMALLINT _Index,
		SQLSMALLINT _InputOutputType,
		SQLSMALLINT _ValueType,
		SQLSMALLINT _ParameterType,
		SQLULEN _ColumnSize,
		SQLSMALLINT _DecimalDigits,
		SQLPOINTER _Value,
		SQLLEN _BufferLength,
		SQLLEN* _Indicator);

	bool BindColumn(SQLUSMALLINT _Index, SQLSMALLINT _TargetType, SQLPOINTER _Value, SQLLEN _BufferLength, SQLLEN* _Indicator);

	bool ExecuteDirect(const wstring& _Query);
	bool GetColumnCount(SQLSMALLINT& _ColumnCount);
	SQLRETURN Fetch();
	SQLRETURN NextResult();
	bool CompleteResults();

	const SDBError& GetLastError() const { return m_LastError; }

private:
	void SetStatementError(EDBErrorCode _Code, const wchar_t* _Operation);

private:
	SQLHSTMT m_StatementHandle;
	SDBError m_LastError;
};
