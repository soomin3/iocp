#include "pch.h"
#include "AuthProcedures.h"
#include "ProcedureBinder.h"

const wchar_t* CProcedure_UserLogin::GetProcedureName() const
{
	return L"dbo.tp_UserLogin";
}

bool CProcedure_UserLogin::BindParameters(CProcedureBinder& _Binder)
{
	return _Binder.Bind(ResultCode) &&
		_Binder.Bind(ID) &&
		_Binder.Bind(PW) &&
		_Binder.Bind(AccountID) &&
		_Binder.Bind(Nickname);
}

bool CProcedure_UserLogin::ValidateResult(wstring& _ErrorMessage)
{
	if (ResultCode.IsNull())
	{
		_ErrorMessage = L"dbo.tp_UserLogin returned a NULL result code";
		return false;
	}

	if (ResultCode.Value == 0 && AccountID.IsNull())
	{
		_ErrorMessage = L"dbo.tp_UserLogin returned a NULL account ID for a successful login";
		return false;
	}

	return true;
}

const wchar_t* CProcedure_CreateNickname::GetProcedureName() const
{
	return L"dbo.tp_UserCreateNickname";
}

bool CProcedure_CreateNickname::BindParameters(CProcedureBinder& _Binder)
{
	return _Binder.Bind(ResultCode) &&
		_Binder.Bind(AccountID) &&
		_Binder.Bind(Nickname);
}

bool CProcedure_CreateNickname::ValidateResult(wstring& _ErrorMessage)
{
	if (ResultCode.IsNull())
	{
		_ErrorMessage = L"dbo.tp_UserCreateNickname returned a NULL result code";
		return false;
	}

	return true;
}
