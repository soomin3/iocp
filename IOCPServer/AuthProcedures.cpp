#include "pch.h"
#include "AuthProcedures.h"

CProcedure_UserLogin::ResultType CProcedure_UserLogin::Execute(SQLHDBC _ConnectionHandle)
{
	ResultType Result;
	wstring ID;
	wstring PW;

	if (!Utf8ToWide(m_ID, ID) || !Utf8ToWide(m_PW, PW))
	{
		return Result;
	}

	int32_t ResultCode = -1;
	int64_t AccountID = 0;

	SQLLEN ResultCodeIndicator = 0;
	SQLLEN IDIndicator = SQL_NTS;
	SQLLEN PWIndicator = SQL_NTS;
	SQLLEN AccountIDIndicator = 0;

	const bool Executed = ExecuteProcedure(_ConnectionHandle, L"{? = CALL dbo.tp_UserLogin(?, ?, ?)}", [&](CDBStatement& _Statement)
		{
			return
				_Statement.BindOutput(1, ResultCode, ResultCodeIndicator) &&
				_Statement.BindInputWString(2, ID, 64, IDIndicator) &&
				_Statement.BindInputWString(3, PW, 64, PWIndicator) &&
				_Statement.BindOutput(4, AccountID, AccountIDIndicator);
		});

	if (!Executed || ResultCodeIndicator == SQL_NULL_DATA)
	{
		return Result;
	}

	if (ResultCode == 0 && AccountIDIndicator == SQL_NULL_DATA)
	{
		return Result;
	}

	Result.Executed = true;
	Result.ResultCode = ResultCode;

	if (ResultCode == 0)
	{
		Result.AccountID = AccountID;
	}

	return Result;
}
