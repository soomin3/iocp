#pragma once
#include <sql.h>
#include <sqlext.h>
#include <string>

using namespace std;

enum class EDBErrorCode
{
	None,
	InvalidConnection,
	StatementAllocationFailed,
	ParameterPreparationFailed,
	ParameterBindFailed,
	ExecuteFailed,
	FetchFailed,
	ResultCompletionFailed,
	InvalidResult
};

struct SDBError
{
	EDBErrorCode Code = EDBErrorCode::None;
	wstring Operation;
	wstring SqlState;
	SQLINTEGER NativeError = 0;
	wstring Message;

	bool HasError() const
	{
		return Code != EDBErrorCode::None;
	}
};

SDBError MakeDBError(EDBErrorCode _Code, const wchar_t* _Operation, const wchar_t* _Message);

SDBError ReadOdbcError(EDBErrorCode _Code, SQLSMALLINT _HandleType, SQLHANDLE _Handle, const wchar_t* _Operation);
