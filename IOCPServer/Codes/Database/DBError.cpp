#include "pch.h"
#include "DBError.h"
#include <algorithm>

SDBError MakeDBError(EDBErrorCode _Code, const wchar_t* _Operation, const wchar_t* _Message)
{
	SDBError Error;
	Error.Code = _Code;

	if (_Operation != nullptr)
	{
		Error.Operation = _Operation;
	}

	if (_Message != nullptr)
	{
		Error.Message = _Message;
	}

	return Error;
}

SDBError ReadOdbcError(EDBErrorCode _Code, SQLSMALLINT _HandleType, SQLHANDLE _Handle, const wchar_t* _Operation)
{
	SDBError Error = MakeDBError(_Code, _Operation, L"ODBC operation failed");

	if (_Handle == SQL_NULL_HANDLE)
	{
		return Error;
	}

	for (SQLSMALLINT Record = 1; ; ++Record)
	{
		SQLWCHAR SqlState[6] = {};
		SQLINTEGER NativeError = 0;
		SQLWCHAR Message[1024] = {};
		SQLSMALLINT MessageLength = 0;

		const SQLRETURN Result = SQLGetDiagRecW(_HandleType, _Handle, Record, SqlState, &NativeError, Message, static_cast<SQLSMALLINT>(size(Message)), &MessageLength);

		if (Result == SQL_NO_DATA || !SQL_SUCCEEDED(Result))
		{
			break;
		}

		if (Record == 1)
		{
			Error.SqlState = reinterpret_cast<const wchar_t*>(SqlState);
			Error.NativeError = NativeError;
			Error.Message.clear();
		}
		else
		{
			Error.Message += L" | ";
		}

		const size_t SafeMessageLength = MessageLength > 0 ? (min)(static_cast<size_t>(MessageLength), size(Message) - 1): 0;

		Error.Message.append(reinterpret_cast<const wchar_t*>(Message),	SafeMessageLength);
	}

	wcerr
		<< L"[ODBC] " << Error.Operation
		<< L" SQLSTATE=" << Error.SqlState
		<< L" NativeError=" << Error.NativeError
		<< L" Message=" << Error.Message
		<< endl;

	return Error;
}
