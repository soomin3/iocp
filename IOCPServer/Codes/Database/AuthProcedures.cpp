#include "pch.h"
#include "AuthProcedures.h"

#include "DBParameter.h"
#include "StoredProcedure.h"

bool ExecuteUserLogin(CDBConnection& _Connection, string _ID, string _PW, SDBUserLoginResult& _Result)
{
	_Result = {};

	TDBReturn<int32_t> ResultCode;
	TDBInputString<64> ID(move(_ID));
	TDBInputString<64> PW(move(_PW));
	TDBOutput<int64_t> AccountID;
	TDBOutputString<16> Nickname;

	CStoredProcedure Procedure;
	if (!Procedure.Execute(_Connection, L"dbo.tp_UserLogin", { &ResultCode, &ID, &PW, &AccountID, &Nickname }))
	{
		_Result.Error = Procedure.GetError();
		return false;
	}

	if (ResultCode.IsNull())
	{
		_Result.Error = MakeDBError(EDBErrorCode::InvalidResult, L"dbo.tp_UserLogin", L"The procedure returned a NULL result code");
		return false;
	}

	if (ResultCode.Value == 0 && AccountID.IsNull())
	{
		_Result.Error = MakeDBError(EDBErrorCode::InvalidResult, L"dbo.tp_UserLogin", L"The procedure returned a NULL account ID for a successful login");
		return false;
	}

	_Result.ResultCode = ResultCode.Value;
	_Result.HasAccountID = ResultCode.Value == 0 && !AccountID.IsNull();
	_Result.AccountID = _Result.HasAccountID ? AccountID.Value : 0;
	_Result.HasNickname = ResultCode.Value == 0 && !Nickname.IsNull() && !Nickname.Value.empty();

	if (_Result.HasNickname)
	{
		_Result.Nickname = move(Nickname.Value);
	}

	return true;
}

bool ExecuteCreateNickname(CDBConnection& _Connection, int64_t _AccountID, string _Nickname, SDBCreateNicknameResult& _Result)
{
	_Result = {};

	TDBReturn<int32_t> ResultCode;
	TDBInput<int64_t> AccountID(_AccountID);
	TDBInputString<16> Nickname(move(_Nickname));

	CStoredProcedure Procedure;
	if (!Procedure.Execute(_Connection, L"dbo.tp_UserCreateNickname", { &ResultCode, &AccountID, &Nickname }))
	{
		_Result.Error = Procedure.GetError();
		return false;
	}

	if (ResultCode.IsNull())
	{
		_Result.Error = MakeDBError(EDBErrorCode::InvalidResult, L"dbo.tp_UserCreateNickname", L"The procedure returned a NULL result code");
		return false;
	}

	_Result.ResultCode = ResultCode.Value;
	return true;
}
