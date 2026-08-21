#include "pch.h"
#include "DBStatement.h"
#include <iterator>

bool CDBStatement::BindInputWString(SQLUSMALLINT _Index, wstring& _Value, SQLULEN _ColumnSize, SQLLEN& _Indicator)
{
    if (_Value.size() > _ColumnSize)
    {
        return false;
    }

    _Indicator = SQL_NTS;

    return SQL_SUCCEEDED(SQLBindParameter(m_StatementHandle, _Index, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, _ColumnSize, 0, const_cast<wchar_t*>(_Value.c_str()), static_cast<SQLLEN>((_Value.size() + 1) * sizeof(wchar_t)), &_Indicator));
}

void CDBStatement::PrintOdbcDiagnostics(SQLSMALLINT _HandleType, SQLHANDLE _Handle, const wchar_t* _Operation)
{
    if (_Handle == SQL_NULL_HANDLE)
    {
        return;
    }

    for (SQLSMALLINT Record = 1; ; ++Record)
    {
        SQLWCHAR SqlState[6] = {};
        SQLINTEGER NativeError = 0;
        SQLWCHAR Message[1024] = {};
        SQLSMALLINT MessageLength = 0;

        const SQLRETURN Result = SQLGetDiagRecW(_HandleType, _Handle, Record, SqlState, &NativeError, Message, static_cast<SQLSMALLINT>(size(Message)), &MessageLength);

        if (Result == SQL_NO_DATA)
        {
            break;
        }

        if (!SQL_SUCCEEDED(Result))
        {
            break;
        }

        wcerr
            << L"[ODBC] " << _Operation
            << L" SQLSTATE="
            << reinterpret_cast<const wchar_t*>(SqlState)
            << L" NativeError=" << NativeError
            << L" Message="
            << reinterpret_cast<const wchar_t*>(Message)
            << endl;
    }
}
