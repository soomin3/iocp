#pragma once
#include <cstdint>
#include "DBParameter.h"
#include "StoredProcedure.h"

using namespace std;

class CProcedure_UserLogin final : public CStoredProcedure
{
public:
	TDBReturn<int32_t> ResultCode;
	TDBInputString<64> ID;
	TDBInputString<64> PW;
	TDBOutput<int64_t> AccountID;
	TDBOutputString<16> Nickname;

protected:
	const wchar_t* GetProcedureName() const override;
	bool BindParameters(CProcedureBinder& _Binder) override;
	bool ValidateResult(wstring& _ErrorMessage) override;
};

class CProcedure_CreateNickname final : public CStoredProcedure
{
public:
	TDBReturn<int32_t> ResultCode;
	TDBInput<int64_t> AccountID;
	TDBInputString<16> Nickname;

protected:
	const wchar_t* GetProcedureName() const override;
	bool BindParameters(CProcedureBinder& _Binder) override;
	bool ValidateResult(wstring& _ErrorMessage) override;
};
