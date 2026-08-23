#include "pch.h"
#include "DBStatement.h"

CDBStatement::CDBStatement(SQLHDBC _ConnectionHandle) : m_StatementHandle(SQL_NULL_HSTMT)
{
	if (_ConnectionHandle == SQL_NULL_HDBC)
	{
		m_LastError = MakeDBError(EDBErrorCode::InvalidConnection, L"CDBStatement::CDBStatement", L"The connection handle is invalid");
		return;
	}

	const SQLRETURN Result = SQLAllocHandle(SQL_HANDLE_STMT, _ConnectionHandle, &m_StatementHandle);

	if (!SQL_SUCCEEDED(Result))
	{
		m_StatementHandle = SQL_NULL_HSTMT;
		m_LastError = ReadOdbcError(EDBErrorCode::StatementAllocationFailed, SQL_HANDLE_DBC, _ConnectionHandle, L"SQLAllocHandle(STMT)");
		return;
	}

	constexpr SQLULEN QUERY_TIMEOUT_SECONDS = 5;
	const SQLRETURN TimeoutResult = SQLSetStmtAttr(m_StatementHandle, SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(QUERY_TIMEOUT_SECONDS), 0);
	if (!SQL_SUCCEEDED(TimeoutResult))
	{
		m_LastError = ReadOdbcError(EDBErrorCode::StatementAllocationFailed, SQL_HANDLE_STMT, m_StatementHandle, L"SQLSetStmtAttr(SQL_ATTR_QUERY_TIMEOUT)");
		SQLFreeHandle(SQL_HANDLE_STMT, m_StatementHandle);
		m_StatementHandle = SQL_NULL_HSTMT;
	}
}

CDBStatement::~CDBStatement()
{
	if (m_StatementHandle != SQL_NULL_HSTMT)
	{
		SQLFreeHandle(SQL_HANDLE_STMT, m_StatementHandle);
		m_StatementHandle = SQL_NULL_HSTMT;
	}
}

bool CDBStatement::BindParameter(
	SQLUSMALLINT _Index,
	SQLSMALLINT _InputOutputType,
	SQLSMALLINT _ValueType,
	SQLSMALLINT _ParameterType,
	SQLULEN _ColumnSize,
	SQLSMALLINT _DecimalDigits,
	SQLPOINTER _Value,
	SQLLEN _BufferLength,
	SQLLEN* _Indicator)
{
	if (!IsValid())
	{
		return false;
	}

	const SQLRETURN Result = SQLBindParameter(
		m_StatementHandle,
		_Index,
		_InputOutputType,
		_ValueType,
		_ParameterType,
		_ColumnSize,
		_DecimalDigits,
		_Value,
		_BufferLength,
		_Indicator);

	if (!SQL_SUCCEEDED(Result))
	{
		SetStatementError(EDBErrorCode::ParameterBindFailed, L"SQLBindParameter");
		return false;
	}

	return true;
}

bool CDBStatement::BindColumn(SQLUSMALLINT _Index, SQLSMALLINT _TargetType, SQLPOINTER _Value, SQLLEN _BufferLength, SQLLEN* _Indicator)
{
	if (!IsValid())
	{
		return false;
	}

	const SQLRETURN Result = SQLBindCol(m_StatementHandle, _Index, _TargetType, _Value, _BufferLength, _Indicator);

	if (!SQL_SUCCEEDED(Result))
	{
		SetStatementError(EDBErrorCode::ParameterBindFailed, L"SQLBindCol");
		return false;
	}

	return true;
}

bool CDBStatement::ExecuteDirect(const wstring& _Query)
{
	if (!IsValid() || _Query.empty())
	{
		return false;
	}

	const SQLRETURN Result = SQLExecDirectW(m_StatementHandle, (SQLWCHAR*)_Query.c_str(), SQL_NTS);

	if (!SQL_SUCCEEDED(Result))
	{
		SetStatementError(EDBErrorCode::ExecuteFailed, L"SQLExecDirectW");
		return false;
	}

	return true;
}

bool CDBStatement::GetColumnCount(SQLSMALLINT& _ColumnCount)
{
	if (!IsValid())
	{
		return false;
	}

	const SQLRETURN Result = SQLNumResultCols(m_StatementHandle, &_ColumnCount);

	if (!SQL_SUCCEEDED(Result))
	{
		SetStatementError(EDBErrorCode::FetchFailed, L"SQLNumResultCols");
		return false;
	}

	return true;
}

SQLRETURN CDBStatement::Fetch()
{
	if (!IsValid())
	{
		return SQL_ERROR;
	}

	const SQLRETURN Result = SQLFetch(m_StatementHandle);

	if (Result != SQL_NO_DATA && !SQL_SUCCEEDED(Result))
	{
		SetStatementError(EDBErrorCode::FetchFailed, L"SQLFetch");
	}

	return Result;
}

SQLRETURN CDBStatement::NextResult()
{
	if (!IsValid())
	{
		return SQL_ERROR;
	}

	const SQLRETURN Result = SQLMoreResults(m_StatementHandle);

	if (Result != SQL_NO_DATA && !SQL_SUCCEEDED(Result))
	{
		SetStatementError(EDBErrorCode::ResultCompletionFailed, L"SQLMoreResults");
	}

	return Result;
}

bool CDBStatement::CompleteResults()
{
	while (true)
	{
		const SQLRETURN Result = NextResult();

		if (Result == SQL_NO_DATA)
		{
			return true;
		}

		if (!SQL_SUCCEEDED(Result))
		{
			return false;
		}
	}
}

void CDBStatement::SetStatementError(EDBErrorCode _Code, const wchar_t* _Operation)
{
	m_LastError = ReadOdbcError(_Code, SQL_HANDLE_STMT, m_StatementHandle, _Operation);
}
