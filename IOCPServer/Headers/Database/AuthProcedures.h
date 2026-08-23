#pragma once

#include <cstdint>
#include <string>

#include "DBError.h"

using namespace std;

class CDBConnection;

struct SDBUserLoginResult
{
	int32_t ResultCode = -1;
	bool HasAccountID = false;
	int64_t AccountID = 0;
	bool HasNickname = false;
	string Nickname;
	SDBError Error;
};

struct SDBCreateNicknameResult
{
	int32_t ResultCode = -1;
	SDBError Error;
};

bool ExecuteUserLogin(CDBConnection& _Connection, string _ID, string _PW, SDBUserLoginResult& _Result);

bool ExecuteCreateNickname(CDBConnection& _Connection, int64_t _AccountID, string _Nickname, SDBCreateNicknameResult& _Result);
