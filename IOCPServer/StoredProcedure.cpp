#include "pch.h"
#include "StoredProcedure.h"

bool CStoredProcedure::Utf8ToWide(const std::string& _Source, std::wstring& _Destination)
{
	if (_Source.empty())
	{
		_Destination.clear();
		return true;
	}

	const int32_t Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _Source.data(), static_cast<int32_t>(_Source.size()),	nullptr, 0);

	if (Length <= 0)
	{
		return false;
	}

	_Destination.resize(Length);

	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _Source.data(), static_cast<int32_t>(_Source.size()), _Destination.data(), Length) == Length;
}

bool CStoredProcedure::ExecuteProcedure(SQLHDBC _ConnectionHandle, const wchar_t* _Query, const StatementAction& _BindParameters)
{
	return ExecuteProcedure(_ConnectionHandle, _Query, _BindParameters, {});
}

bool CStoredProcedure::ExecuteProcedure(SQLHDBC _ConnectionHandle, const wchar_t* _Query, const StatementAction& _BindParameters, const StatementAction& _ReadResultRows)
{
	if (_ConnectionHandle == SQL_NULL_HDBC || _Query == nullptr || !_BindParameters)
	{
		return false;
	}

	CDBStatement Statement(_ConnectionHandle);

	if (!Statement.IsValid())
	{
		return false;
	}

	if (!_BindParameters(Statement))
	{
		return false;
	}

	if (!Statement.ExecuteDirect(_Query))
	{
		return false;
	}

	if (_ReadResultRows && !_ReadResultRows(Statement))
	{
		return false;
	}

	return Statement.CompleteResults();
}
