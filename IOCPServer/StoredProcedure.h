#pragma once
#include <string>
#include <sql.h>
#include <sqlext.h>
#include "DBStatement.h"

using namespace std;

class CStoredProcedure
{
public:
	CStoredProcedure();

protected:
	virtual void Init();

	template<typename PROC>
	bool ExecuteProcedure(SQLHDBC _ConnectionHandle, PROC& _Procedure)
	{
		return false;
	}

};