#pragma once

#include <cstdint>
#include <sql.h>
#include <sqlext.h>
#include <string>

template <typename T>
struct OdbcType;

template<>
struct OdbcType<int16_t>
{
	static constexpr SQLSMALLINT cType = SQL_C_SSHORT;
	static constexpr SQLSMALLINT sqlType = SQL_SMALLINT;
};

template<>
struct OdbcType<int32_t>
{
	static constexpr SQLSMALLINT cType = SQL_C_SLONG;
	static constexpr SQLSMALLINT sqlType = SQL_INTEGER;
};

template<>
struct OdbcType<int64_t>
{
	static constexpr SQLSMALLINT cType = SQL_C_SBIGINT;
	static constexpr SQLSMALLINT sqlType = SQL_BIGINT;
};

template<>
struct OdbcType<uint8_t>
{
	static constexpr SQLSMALLINT cType = SQL_C_UTINYINT;
	static constexpr SQLSMALLINT sqlType = SQL_TINYINT;
};

template<>
struct OdbcType<bool>
{
	static constexpr SQLSMALLINT cType = SQL_C_BIT;
	static constexpr SQLSMALLINT sqlType = SQL_BIT;
};

template<>
struct OdbcType<float>
{
	static constexpr SQLSMALLINT cType = SQL_C_FLOAT;
	static constexpr SQLSMALLINT sqlType = SQL_REAL;
};

template<>
struct OdbcType<double>
{
	static constexpr SQLSMALLINT cType = SQL_C_DOUBLE;
	static constexpr SQLSMALLINT sqlType = SQL_DOUBLE;
};

class CDBStatement
{
public:
	explicit CDBStatement(SQLHDBC _ConnectionHandle);
	~CDBStatement();

	CDBStatement(const CDBStatement&) = delete;
	CDBStatement& operator=(const CDBStatement&) = delete;

	bool IsValid() const { return m_StatementHandle != SQL_NULL_HSTMT; }

	template<typename T>
	bool BindInput(SQLUSMALLINT _Index, T& _Value, SQLLEN& _Indicator)
	{
		const SQLRETURN Result = SQLBindParameter(
			m_StatementHandle,
			_Index,
			SQL_PARAM_INPUT,
			OdbcType<T>::cType,
			OdbcType<T>::sqlType,
			0,
			0,
			&_Value,
			sizeof(T),
			&_Indicator);

		if (!SQL_SUCCEEDED(Result))
		{
			PrintDiagnostics(L"SQLBindParameter(input)");
			return false;
		}

		return true;
	}

	template<typename T>
	bool BindOutput(SQLUSMALLINT _Index, T& _Value, SQLLEN& _Indicator)
	{
		const SQLRETURN Result = SQLBindParameter(
			m_StatementHandle,
			_Index,
			SQL_PARAM_OUTPUT,
			OdbcType<T>::cType,
			OdbcType<T>::sqlType,
			0,
			0,
			&_Value,
			sizeof(T),
			&_Indicator);

		if (!SQL_SUCCEEDED(Result))
		{
			PrintDiagnostics(L"SQLBindParameter(output)");
			return false;
		}

		return true;
	}

	template<typename T>
	bool BindColumn(SQLUSMALLINT _Index, T& _Value, SQLLEN& _Indicator)
	{
		const SQLRETURN Result = SQLBindCol(
			m_StatementHandle,
			_Index,
			OdbcType<T>::cType,
			&_Value,
			sizeof(T),
			&_Indicator);

		if (!SQL_SUCCEEDED(Result))
		{
			PrintDiagnostics(L"SQLBindCol");
			return false;
		}

		return true;
	}

	bool BindInputWString(SQLUSMALLINT _Index, const std::wstring& _Value, SQLULEN _ColumnSize, SQLLEN& _Indicator);
	bool BindOutputWString(SQLUSMALLINT _Index, wchar_t* _Buffer, SQLULEN _BufferCharacterCount, SQLLEN& _Indicator);
	bool BindColumnWString(SQLUSMALLINT _Index, wchar_t* _Buffer, SQLULEN _BufferCharacterCount, SQLLEN& _Indicator);

	bool ExecuteDirect(const wchar_t* _Query);
	SQLRETURN Fetch();
	SQLRETURN NextResult();
	bool CompleteResults();

	void PrintDiagnostics(const wchar_t* _Operation) const;

private:
	SQLHSTMT m_StatementHandle;
};
