#include "pch.h"
#include "DBResultReader.h"

CDBResultReader::CDBResultReader(CDBStatement& _Statement)
	: m_Statement(_Statement),
	m_NextIndex(1),
	m_Succeeded(true),
	m_ResultSetReady(false)
{
}

bool CDBResultReader::Bind(CDBColumn& _Column)
{
	if (!MoveToResultSet())
	{
		return false;
	}

	_Column.PrepareForBind();

	if (!_Column.Bind(m_Statement, m_NextIndex))
	{
		m_Succeeded = false;
		m_ErrorMessage = m_Statement.GetLastError().Message;
		return false;
	}

	m_Columns.push_back(&_Column);
	++m_NextIndex;
	return true;
}

bool CDBResultReader::MoveToResultSet()
{
	if (m_ResultSetReady)
	{
		return true;
	}

	while (true)
	{
		SQLSMALLINT ColumnCount = 0;

		if (!m_Statement.GetColumnCount(ColumnCount))
		{
			m_Succeeded = false;
			m_ErrorMessage = m_Statement.GetLastError().Message;
			return false;
		}

		if (ColumnCount > 0)
		{
			m_ResultSetReady = true;
			return true;
		}

		const SQLRETURN Result = m_Statement.NextResult();

		if (Result == SQL_NO_DATA)
		{
			m_Succeeded = false;
			m_ErrorMessage = L"The procedure did not return a SELECT result set";
			return false;
		}

		if (!SQL_SUCCEEDED(Result))
		{
			m_Succeeded = false;
			m_ErrorMessage = m_Statement.GetLastError().Message;
			return false;
		}
	}
}

bool CDBResultReader::Fetch()
{
	if (!m_Succeeded)
	{
		return false;
	}

	const SQLRETURN Result = m_Statement.Fetch();

	if (Result == SQL_NO_DATA)
	{
		return false;
	}

	if (!SQL_SUCCEEDED(Result))
	{
		m_Succeeded = false;
		m_ErrorMessage = m_Statement.GetLastError().Message;
		return false;
	}

	for (CDBColumn* Column : m_Columns)
	{
		if (!Column->CompleteValue())
		{
			m_Succeeded = false;
			m_ErrorMessage = Column->GetErrorMessage();
			return false;
		}
	}

	return true;
}
