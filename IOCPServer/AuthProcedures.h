#pragma once
#include <cstdint>
#include "StoredProcedure.h"
#include "AuthProcedureTypes.h"

using namespace std;

class CProcedure_UserLogin : public CStoredProcedure
{
public:
	SLoginProcedureResult Execute(SQLHDBC _ConnectionHandle, string& _ID, string& _PW);

private:

};