#pragma once
#include <string>
#include "DBConnection.h"
#include "DBError.h"

using namespace std;

class CProcedureBinder;
class CDBResultReader;

class CStoredProcedure
{
public:
	CStoredProcedure() = default;
	virtual ~CStoredProcedure() = default;

	CStoredProcedure(const CStoredProcedure&) = delete;
	CStoredProcedure& operator=(const CStoredProcedure&) = delete;
	CStoredProcedure(CStoredProcedure&&) noexcept = default;
	CStoredProcedure& operator=(CStoredProcedure&&) noexcept = default;

	bool Execute(CDBConnection& _Connection);

	bool Succeeded() const { return m_Succeeded; }
	const SDBError& GetError() const { return m_Error; }
	const wstring& GetErrorMessage() const { return m_Error.Message; }

protected:
	virtual const wchar_t* GetProcedureName() const = 0;
	virtual bool BindParameters(CProcedureBinder& _Binder) = 0;

	virtual void ResetResultRows() {}
	virtual bool ReadResultRows(CDBResultReader&) { return true; }
	virtual bool ValidateResult(wstring&) { return true; }

private:
	void SetError(EDBErrorCode _Code, const wchar_t* _Operation, const wstring& _Message);

private:
	bool m_Succeeded = false;
	SDBError m_Error;
};
