#pragma once

#include <functional>
#include <initializer_list>
#include <string>

#include "DBConnection.h"
#include "DBError.h"

using namespace std;

class CDBParameter;
class CDBResultReader;

class CStoredProcedure final
{
public:
	using TResultReader = function<bool(CDBResultReader&)>;
	bool Execute(CDBConnection& _Connection, const wchar_t* _ProcedureName, initializer_list<CDBParameter*> _Parameters, const TResultReader& _ReadResultRows = {});
	const SDBError& GetError() const { return m_Error; }

private:
	void SetError(EDBErrorCode _Code, const wchar_t* _Operation, const wstring& _Message);

private:
	SDBError m_Error;
};
