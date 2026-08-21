#pragma once
#include <cstdint>
#include <sql.h>
#include <sqlext.h>
#include <string>

using namespace std;

template <typename T>
struct OdbcType;

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
struct OdbcType<wchar_t>
{
    static constexpr SQLSMALLINT cType = SQL_C_WCHAR;
    static constexpr SQLSMALLINT sqlType = SQL_WVARCHAR;
};

class CDBStatement
{
public:
    explicit CDBStatement(SQLHSTMT _StatementHandle) : m_StatementHandle(_StatementHandle)
    {
    }

    template<typename T>
    bool BindInput(SQLUSMALLINT _Index, T& _Value, SQLLEN& _Ind)
    {
        return SQL_SUCCEEDED(SQLBindParameter(m_StatementHandle, _Index, SQL_PARAM_INPUT, OdbcType<T>::cType, OdbcType<T>::sqlType, 0, 0, &_Value, sizeof(T), &_Ind));
    }

    template<typename T>
    bool BindOutput(SQLUSMALLINT _Index, T& _Value, SQLLEN& _Ind)
    {
        return SQL_SUCCEEDED(SQLBindParameter(m_StatementHandle, _Index, SQL_PARAM_OUTPUT, OdbcType<T>::cType, OdbcType<T>::sqlType, 0, 0, &_Value, sizeof(T), &_Ind));
    }

    template<typename T>
    bool BindColumn(SQLUSMALLINT _Index, T& _Value, SQLLEN& _Ind)
    {
        return SQL_SUCCEEDED(SQLBindCol(m_StatementHandle, _Index, OdbcType<T>::cType, &_Value, sizeof(T), &_Ind));
    }

    bool BindInputWString(SQLUSMALLINT _Index, wstring& _Value, SQLULEN _ColumnSize, SQLLEN& _Indicator);

    void PrintOdbcDiagnostics(SQLSMALLINT _HandleType, SQLHANDLE _Handle, const wchar_t* _Operation);

private:
    SQLHSTMT m_StatementHandle;
};
