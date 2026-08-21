#pragma once

#include <functional>
#include <string>
#include <sql.h>
#include <sqlext.h>
#include "DBStatement.h"

class CStoredProcedure
{
protected:
	using StatementAction = std::function<bool(CDBStatement&)>;
	static bool Utf8ToWide(const std::string& _Source, std::wstring& _Destination);

	bool ExecuteProcedure(
		SQLHDBC _ConnectionHandle,
		const wchar_t* _Query,
		const StatementAction& _BindParameters);

	bool ExecuteProcedure(
		SQLHDBC _ConnectionHandle,
		const wchar_t* _Query,
		const StatementAction& _BindParameters,
		const StatementAction& _ReadResultRows);
};
