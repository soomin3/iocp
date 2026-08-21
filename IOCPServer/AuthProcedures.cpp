#include "pch.h"
#include "AuthProcedures.h"
#include "DBStatement.h"

namespace
{
    bool Utf8ToWide(string& _Source, wstring& _Destination)
    {
        if (_Source.empty())
        {
            _Destination.clear();
            return true;
        }

        int32_t Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _Source.data(), static_cast<int32_t>(_Source.size()), nullptr, 0);

        if (Length <= 0)
        {
            return false;
        }

        _Destination.resize(Length);

        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _Source.data(), static_cast<int32_t>(_Source.size()), _Destination.data(), Length) == Length;
    }
}

SLoginProcedureResult CProcedure_UserLogin::Execute(SQLHDBC _ConnectionHandle, string& _ID, string& _PW)
{
    SLoginProcedureResult Result;

    if (_ConnectionHandle == SQL_NULL_HDBC)
    {
        return Result;
    }

    wstring ID, PW;

    if (!Utf8ToWide(_ID, ID) || !Utf8ToWide(_PW, PW))
    {
        return Result;
    }

    SQLHSTMT StatementHandle = SQL_NULL_HSTMT;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, _ConnectionHandle, &StatementHandle)))
    {
        return Result;
    }

    do
    {
        CDBStatement Statement(StatementHandle);

        int32_t ResultCode = -1;
        int64_t AccountID = 0;

        SQLLEN ResultCodeIndicator = 0;
        SQLLEN IDIndicator = SQL_NTS;
        SQLLEN PWIndicator = SQL_NTS;
        SQLLEN AccountIDIndicator = 0;

        // {? = CALL dbo.tp_UserLogin(?, ?, ?)}
        if (!Statement.BindOutput(1, ResultCode, ResultCodeIndicator))
        {
            Statement.PrintOdbcDiagnostics(SQL_HANDLE_STMT, StatementHandle, L"Bind return code");
            break;
        }

        if (!Statement.BindInputWString(2, ID, 64, IDIndicator))
        {
            break;
        }

        if (!Statement.BindInputWString(3, PW, 64, PWIndicator))
        {
            break;
        }

        if (!Statement.BindOutput(4, AccountID, AccountIDIndicator))
        {
            break;
        }

        SQLWCHAR Query[] = L"{? = CALL dbo.tp_UserLogin(?, ?, ?)}";

        SQLRETURN OdbcResult = SQLExecDirectW(StatementHandle, Query, SQL_NTS);

        if (!SQL_SUCCEEDED(OdbcResult))
        {
            break;
        }

        bool ResultsCompleted = false;

        while (true)
        {
            OdbcResult = SQLMoreResults(StatementHandle);

            if (OdbcResult == SQL_NO_DATA)
            {
                ResultsCompleted = true;
                break;
            }

            if (!SQL_SUCCEEDED(OdbcResult))
            {
                break;
            }
        }

        if (!ResultsCompleted)
        {
            break;
        }

        if (ResultCodeIndicator == SQL_NULL_DATA)
        {
            break;
        }

        if (ResultCode == 0 &&
            AccountIDIndicator == SQL_NULL_DATA)
        {
            break;
        }

        Result.Executed = true;
        Result.ResultCode = ResultCode;

        if (ResultCode == 0)
        {
            Result.AccountID = AccountID;
        }
    } while (false);

    SQLFreeHandle(SQL_HANDLE_STMT, StatementHandle);
    return Result;
}
