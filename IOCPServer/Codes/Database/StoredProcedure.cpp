#include "pch.h"
#include "StoredProcedure.h"

#include "DBParameter.h"
#include "DBResultReader.h"
#include "DBStatement.h"

namespace
{
	wstring BuildProcedureCall(const wchar_t* _ProcedureName, size_t _ParameterCount, bool _HasReturnValue)
	{
		wstring Query = _HasReturnValue ? L"{? = CALL " : L"{CALL ";
		Query += _ProcedureName;

		if (_ParameterCount > 0)
		{
			Query += L"(";

			for (size_t Index = 0; Index < _ParameterCount; ++Index)
			{
				if (Index > 0)
				{
					Query += L", ";
				}

				Query += L"?";
			}

			Query += L")";
		}

		Query += L"}";
		return Query;
	}
}

bool CStoredProcedure::Execute(CDBConnection& _Connection, const wchar_t* _ProcedureName, initializer_list<CDBParameter*> _Parameters, const TResultReader& _ReadResultRows)
{
	m_Error = {};

	if (!_Connection.IsConnected())
	{
		SetError(EDBErrorCode::InvalidConnection, L"CStoredProcedure::Execute", L"The DB connection is not connected");
		return false;
	}

	if (_ProcedureName == nullptr || _ProcedureName[0] == L'\0')
	{
		SetError(EDBErrorCode::ParameterPreparationFailed, L"CStoredProcedure::Execute", L"The procedure name is empty");
		return false;
	}

	CDBStatement Statement(_Connection.GetHandle());

	if (!Statement.IsValid())
	{
		m_Error = Statement.GetLastError();
		return false;
	}

	bool HasReturnValue = false;
	SQLUSMALLINT ParameterIndex = 1;

	for (CDBParameter* Parameter : _Parameters)
	{
		if (Parameter == nullptr)
		{
			SetError(EDBErrorCode::ParameterPreparationFailed, L"CStoredProcedure::Execute", L"A procedure parameter is null");
			return false;
		}

		if (Parameter->GetDirection() == EDBParameterDirection::ReturnValue)
		{
			if (ParameterIndex != 1 || HasReturnValue)
			{
				SetError(EDBErrorCode::ParameterPreparationFailed, L"CStoredProcedure::Execute", L"A return value must be the first and only return parameter");
				return false;
			}

			HasReturnValue = true;
		}

		if (!Parameter->PrepareForBind())
		{
			wstring Message = L"Parameter ";
			Message += to_wstring(ParameterIndex);
			Message += L": ";
			Message += Parameter->GetErrorMessage();
			SetError(EDBErrorCode::ParameterPreparationFailed, L"CStoredProcedure::Execute", Message);
			return false;
		}

		if (!Parameter->Bind(Statement, ParameterIndex))
		{
			m_Error = Statement.GetLastError();
			return false;
		}

		++ParameterIndex;
	}

	const size_t ParameterCount = _Parameters.size() - (HasReturnValue ? 1 : 0);
	const wstring Query = BuildProcedureCall(_ProcedureName, ParameterCount, HasReturnValue);

	if (!Statement.ExecuteDirect(Query))
	{
		m_Error = Statement.GetLastError();
		return false;
	}

	if (_ReadResultRows)
	{
		CDBResultReader Reader(Statement);

		if (!_ReadResultRows(Reader) || !Reader.Succeeded())
		{
			if (Statement.GetLastError().HasError())
			{
				m_Error = Statement.GetLastError();
				return false;
			}

			SetError(EDBErrorCode::FetchFailed,	L"CStoredProcedure::Execute", Reader.GetErrorMessage().empty() ? L"The procedure result rows could not be read" : Reader.GetErrorMessage());
			return false;
		}
	}

	if (!Statement.CompleteResults())
	{
		m_Error = Statement.GetLastError();
		return false;
	}

	for (CDBParameter* Parameter : _Parameters)
	{
		if (!Parameter->CompleteValue())
		{
			SetError(EDBErrorCode::ResultCompletionFailed, L"CStoredProcedure::Execute", Parameter->GetErrorMessage());
			return false;
		}
	}

	return true;
}

void CStoredProcedure::SetError(EDBErrorCode _Code, const wchar_t* _Operation, const wstring& _Message)
{
	m_Error = MakeDBError(_Code, _Operation, _Message.c_str());
}
