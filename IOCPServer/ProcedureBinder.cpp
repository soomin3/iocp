#include "pch.h"
#include "ProcedureBinder.h"

CProcedureBinder::CProcedureBinder(CDBStatement& _Statement): m_Statement(_Statement), m_NextIndex(1), m_HasReturnValue(false)
{
}

bool CProcedureBinder::Bind(CDBParameter& _Parameter)
{
	const EDBParameterDirection Direction = _Parameter.GetDirection();

	if (Direction == EDBParameterDirection::ReturnValue)
	{
		if (m_NextIndex != 1 || m_HasReturnValue)
		{
			m_ErrorMessage = L"A return value must be the first and only return parameter";
			return false;
		}

		m_HasReturnValue = true;
	}

	if (!_Parameter.PrepareForBind())
	{
		m_ErrorMessage = L"Parameter ";
		m_ErrorMessage += to_wstring(m_NextIndex);
		m_ErrorMessage += L": ";
		m_ErrorMessage += _Parameter.GetErrorMessage();
		return false;
	}

	if (!_Parameter.Bind(m_Statement, m_NextIndex))
	{
		m_ErrorMessage = m_Statement.GetLastError().Message;
		return false;
	}

	m_Parameters.push_back(&_Parameter);
	++m_NextIndex;
	return true;
}

bool CProcedureBinder::CompleteParameters()
{
	for (CDBParameter* Parameter : m_Parameters)
	{
		if (!Parameter->CompleteValue())
		{
			m_ErrorMessage = Parameter->GetErrorMessage();
			return false;
		}
	}

	return true;
}

wstring CProcedureBinder::BuildCallQuery(const wchar_t* _ProcedureName)
{
	if (_ProcedureName == nullptr || _ProcedureName[0] == L'\0')
	{
		m_ErrorMessage = L"The procedure name is empty";
		return {};
	}

	const size_t ParameterCount = m_Parameters.size() - (m_HasReturnValue ? 1 : 0);

	wstring Query = L"{";

	if (m_HasReturnValue)
	{
		Query += L"? = ";
	}

	Query += L"CALL ";
	Query += _ProcedureName;

	if (ParameterCount > 0)
	{
		Query += L"(";

		for (size_t Index = 0; Index < ParameterCount; ++Index)
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

