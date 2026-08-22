#include "pch.h"
#include "StoredProcedure.h"
#include "DBResultReader.h"
#include "DBStatement.h"
#include "ProcedureBinder.h"

bool CStoredProcedure::Execute(CDBConnection& _Connection)
{
	m_Succeeded = false;
	m_Error = {};
	ResetResultRows();

	if (!_Connection.IsConnected())
	{
		SetError(EDBErrorCode::InvalidConnection, L"CStoredProcedure::Execute", L"The DB connection is not connected");
		return false;
	}

	CDBStatement Statement(_Connection.GetHandle());

	if (!Statement.IsValid())
	{
		m_Error = Statement.GetLastError();
		return false;
	}

	CProcedureBinder Binder(Statement);

	if (!BindParameters(Binder))
	{
		if (Statement.GetLastError().HasError())
		{
			m_Error = Statement.GetLastError();
			return false;
		}

		SetError(EDBErrorCode::ParameterPreparationFailed, L"BindParameters", Binder.GetErrorMessage().empty() ? L"The procedure parameter binding failed" : Binder.GetErrorMessage());
		return false;
	}

	const wstring Query = Binder.BuildCallQuery(GetProcedureName());

	if (Query.empty())
	{
		SetError(EDBErrorCode::ParameterPreparationFailed, L"BuildCallQuery", Binder.GetErrorMessage());
		return false;
	}

	if (!Statement.ExecuteDirect(Query))
	{
		m_Error = Statement.GetLastError();
		return false;
	}

	CDBResultReader Reader(Statement);

	if (!ReadResultRows(Reader) || !Reader.Succeeded())
	{
		if (Statement.GetLastError().HasError())
		{
			m_Error = Statement.GetLastError();
			return false;
		}

		SetError(EDBErrorCode::FetchFailed, L"ReadResultRows", Reader.GetErrorMessage().empty() ? L"The procedure result rows could not be read" : Reader.GetErrorMessage());
		return false;
	}

	if (!Statement.CompleteResults())
	{
		m_Error = Statement.GetLastError();
		return false;
	}

	if (!Binder.CompleteParameters())
	{
		SetError(EDBErrorCode::ResultCompletionFailed, L"CompleteParameters", Binder.GetErrorMessage());
		return false;
	}

	wstring ValidationError;

	if (!ValidateResult(ValidationError))
	{
		SetError(EDBErrorCode::InvalidResult, L"ValidateResult", ValidationError.empty() ? L"The procedure returned an invalid result" : ValidationError);
		return false;
	}

	m_Succeeded = true;
	return true;
}

void CStoredProcedure::SetError(EDBErrorCode _Code, const wchar_t* _Operation, const wstring& _Message)
{
	m_Error = MakeDBError(_Code, _Operation, _Message.c_str());
}
