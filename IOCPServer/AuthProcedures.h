#pragma once
#include <string>
#include <utility>
#include "StoredProcedure.h"
#include "AuthProcedureTypes.h"

using namespace std;

class CProcedure_UserLogin : public CStoredProcedure
{
public:
	using ResultType = SLoginProcedureResult;

	CProcedure_UserLogin(string _ID, string _PW): m_ID(move(_ID)), m_PW(move(_PW))
	{
	}

	ResultType Execute(SQLHDBC _ConnectionHandle);

private:
	string m_ID;
	string m_PW;
};
