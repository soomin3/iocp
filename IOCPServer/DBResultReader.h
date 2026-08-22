#pragma once
#include <string>
#include <vector>
#include "DBParameter.h"

using namespace std;

class CDBResultReader
{
public:
	explicit CDBResultReader(CDBStatement& _Statement);

	bool Bind(CDBColumn& _Column);
	bool Fetch();

	bool Succeeded() const { return m_Succeeded; }
	const wstring& GetErrorMessage() const { return m_ErrorMessage; }

private:
	bool MoveToResultSet();

private:
	CDBStatement& m_Statement;
	vector<CDBColumn*> m_Columns;
	SQLUSMALLINT m_NextIndex;
	bool m_Succeeded;
	bool m_ResultSetReady;
	wstring m_ErrorMessage;
};
