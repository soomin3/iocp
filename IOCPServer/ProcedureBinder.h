#pragma once

#include <string>
#include <vector>
#include "DBParameter.h"

using namespace std;

class CProcedureBinder
{
public:
	explicit CProcedureBinder(CDBStatement& _Statement);

	bool Bind(CDBParameter& _Parameter);
	bool CompleteParameters();
	wstring BuildCallQuery(const wchar_t* _ProcedureName);

	const wstring& GetErrorMessage() const { return m_ErrorMessage; }

private:
	CDBStatement& m_Statement;
	vector<CDBParameter*> m_Parameters;
	SQLUSMALLINT m_NextIndex;
	bool m_HasReturnValue;
	wstring m_ErrorMessage;
};
