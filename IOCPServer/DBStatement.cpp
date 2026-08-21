#include "pch.h"
#include "DBStatement.h"

CDBStatement::CDBStatement(SQLHDBC _ConnectionHandle) : m_StatementHandle(SQL_NULL_HSTMT)
{
	if (_ConnectionHandle == SQL_NULL_HDBC)
	{
		return;
	}

	SQLAllocHandle(SQL_HANDLE_STMT, _ConnectionHandle, &m_StatementHandle);
}

CDBStatement::~CDBStatement()
{
	if (m_StatementHandle != SQL_NULL_HSTMT)
	{
		SQLFreeHandle(SQL_HANDLE_STMT, m_StatementHandle);
		m_StatementHandle = SQL_NULL_HSTMT;
	}
}

bool CDBStatement::BindInputWString(SQLUSMALLINT _Index, const std::wstring& _Value, SQLULEN _ColumnSize, SQLLEN& _Indicator)
{
	if (_Value.size() > _ColumnSize)
	{
		return false;
	}

	if (_Indicator != SQL_NULL_DATA)
	{
		_Indicator = SQL_NTS;
	}

	const SQLRETURN Result = SQLBindParameter(
		m_StatementHandle,
		_Index,
		SQL_PARAM_INPUT,
		SQL_C_WCHAR,
		SQL_WVARCHAR,
		_ColumnSize,
		0,
		const_cast<wchar_t*>(_Value.c_str()),
		static_cast<SQLLEN>((_Value.size() + 1) * sizeof(wchar_t)),
		&_Indicator);

	if (!SQL_SUCCEEDED(Result))
	{
		PrintDiagnostics(L"SQLBindParameter(input string)");
		return false;
	}

	return true;
}

bool CDBStatement::BindOutputWString(SQLUSMALLINT _Index, wchar_t* _Buffer, SQLULEN _BufferCharacterCount, SQLLEN& _Indicator)
{
	if (_Buffer == nullptr || _BufferCharacterCount == 0)
	{
		return false;
	}

	const SQLRETURN Result = SQLBindParameter(
		m_StatementHandle,
		_Index,
		SQL_PARAM_OUTPUT,
		SQL_C_WCHAR,
		SQL_WVARCHAR,
		_BufferCharacterCount - 1,
		0,
		_Buffer,
		static_cast<SQLLEN>(_BufferCharacterCount * sizeof(wchar_t)),
		&_Indicator);

	if (!SQL_SUCCEEDED(Result))
	{
		PrintDiagnostics(L"SQLBindParameter(output string)");
		return false;
	}

	return true;
}

bool CDBStatement::BindColumnWString(SQLUSMALLINT _Index, wchar_t* _Buffer, SQLULEN _BufferCharacterCount, SQLLEN& _Indicator)
{
	if (_Buffer == nullptr || _BufferCharacterCount == 0)
	{
		return false;
	}

	const SQLRETURN Result = SQLBindCol(
		m_StatementHandle,
		_Index,
		SQL_C_WCHAR,
		_Buffer,
		static_cast<SQLLEN>(_BufferCharacterCount * sizeof(wchar_t)),
		&_Indicator);

	if (!SQL_SUCCEEDED(Result))
	{
		PrintDiagnostics(L"SQLBindCol(string)");
		return false;
	}

	return true;
}

bool CDBStatement::ExecuteDirect(const wchar_t* _Query)
{
	if (!IsValid() || _Query == nullptr)
	{
		return false;
	}

	const SQLRETURN Result = SQLExecDirectW(
		m_StatementHandle,
		(SQLWCHAR*)_Query,
		SQL_NTS);

	if (!SQL_SUCCEEDED(Result))
	{
		PrintDiagnostics(L"SQLExecDirectW");
		return false;
	}

	return true;
}

SQLRETURN CDBStatement::Fetch()
{
	const SQLRETURN Result = SQLFetch(m_StatementHandle);

	if (Result != SQL_NO_DATA && !SQL_SUCCEEDED(Result))
	{
		PrintDiagnostics(L"SQLFetch");
	}

	return Result;
}

SQLRETURN CDBStatement::NextResult()
{
	const SQLRETURN Result = SQLMoreResults(m_StatementHandle);

	if (Result != SQL_NO_DATA && !SQL_SUCCEEDED(Result))
	{
		PrintDiagnostics(L"SQLMoreResults");
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

void CDBStatement::PrintDiagnostics(const wchar_t* _Operation) const
{
	if (!IsValid())
	{
		return;
	}

	for (SQLSMALLINT Record = 1; ; ++Record)
	{
		SQLWCHAR SqlState[6] = {};
		SQLINTEGER NativeError = 0;
		SQLWCHAR Message[1024] = {};
		SQLSMALLINT MessageLength = 0;

		const SQLRETURN Result = SQLGetDiagRecW(
			SQL_HANDLE_STMT,
			m_StatementHandle,
			Record,
			SqlState,
			&NativeError,
			Message,
			static_cast<SQLSMALLINT>(std::size(Message)),
			&MessageLength);

		if (Result == SQL_NO_DATA || !SQL_SUCCEEDED(Result))
		{
			break;
		}

		std::wcerr
			<< L"[ODBC] " << _Operation
			<< L" SQLSTATE=" << reinterpret_cast<const wchar_t*>(SqlState)
			<< L" NativeError=" << NativeError
			<< L" Message=" << reinterpret_cast<const wchar_t*>(Message)
			<< std::endl;
	}
}
