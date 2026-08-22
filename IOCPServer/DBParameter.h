#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <sql.h>
#include <sqlext.h>
#include <string>
#include <utility>
#include "DBStatement.h"
#include "TextEncoding.h"

using namespace std;

enum class EDBParameterDirection
{
	ReturnValue,
	Input,
	Output,
	InputOutput
};

template<typename T>
struct TOdbcScalarType;

template<>
struct TOdbcScalarType<int16_t>
{
	static constexpr SQLSMALLINT CType = SQL_C_SSHORT;
	static constexpr SQLSMALLINT SQLType = SQL_SMALLINT;
};

template<>
struct TOdbcScalarType<int32_t>
{
	static constexpr SQLSMALLINT CType = SQL_C_SLONG;
	static constexpr SQLSMALLINT SQLType = SQL_INTEGER;
};

template<>
struct TOdbcScalarType<int64_t>
{
	static constexpr SQLSMALLINT CType = SQL_C_SBIGINT;
	static constexpr SQLSMALLINT SQLType = SQL_BIGINT;
};

template<>
struct TOdbcScalarType<uint8_t>
{
	static constexpr SQLSMALLINT CType = SQL_C_UTINYINT;
	static constexpr SQLSMALLINT SQLType = SQL_TINYINT;
};

template<>
struct TOdbcScalarType<bool>
{
	static constexpr SQLSMALLINT CType = SQL_C_BIT;
	static constexpr SQLSMALLINT SQLType = SQL_BIT;
};

template<>
struct TOdbcScalarType<float>
{
	static constexpr SQLSMALLINT CType = SQL_C_FLOAT;
	static constexpr SQLSMALLINT SQLType = SQL_REAL;
};

template<>
struct TOdbcScalarType<double>
{
	static constexpr SQLSMALLINT CType = SQL_C_DOUBLE;
	static constexpr SQLSMALLINT SQLType = SQL_DOUBLE;
};

class CDBParameter
{
public:
	CDBParameter() = default;
	virtual ~CDBParameter() = default;

	CDBParameter(const CDBParameter&) = delete;
	CDBParameter& operator=(const CDBParameter&) = delete;
	CDBParameter(CDBParameter&&) noexcept = default;
	CDBParameter& operator=(CDBParameter&&) noexcept = default;

	virtual EDBParameterDirection GetDirection() const = 0;
	virtual bool PrepareForBind() = 0;
	virtual bool Bind(CDBStatement& _Statement, SQLUSMALLINT _Index) = 0;
	virtual bool CompleteValue() { return true; }

	const wstring& GetErrorMessage() const { return m_ErrorMessage; }

protected:
	void ClearError() { m_ErrorMessage.clear(); }
	void SetError(const wchar_t* _Message)
	{
		m_ErrorMessage = _Message == nullptr ? L"" : _Message;
	}

private:
	wstring m_ErrorMessage;
};

inline SQLSMALLINT GetOdbcParameterDirection(EDBParameterDirection _Direction)
{
	switch (_Direction)
	{
	case EDBParameterDirection::Input:
		return SQL_PARAM_INPUT;
	case EDBParameterDirection::InputOutput:
		return SQL_PARAM_INPUT_OUTPUT;
	case EDBParameterDirection::ReturnValue:
	case EDBParameterDirection::Output:
	default:
		return SQL_PARAM_OUTPUT;
	}
}

template<typename T, EDBParameterDirection Direction>
class TDBScalarParameter final : public CDBParameter
{
public:
	T Value{};

	TDBScalarParameter& operator=(const T& _Value)
	{
		Value = _Value;
		m_IsAssigned = true;
		m_Indicator = 0;
		return *this;
	}

	void SetNull()
	{
		m_IsAssigned = true;
		m_Indicator = SQL_NULL_DATA;
	}

	bool IsNull() const { return m_Indicator == SQL_NULL_DATA; }
	bool IsAssigned() const { return m_IsAssigned; }

	EDBParameterDirection GetDirection() const override
	{
		return Direction;
	}

	bool PrepareForBind() override
	{
		ClearError();

		if constexpr (
			Direction == EDBParameterDirection::Input ||
			Direction == EDBParameterDirection::InputOutput)
		{
			if (!m_IsAssigned)
			{
				SetError(L"A required input parameter was not assigned");
				return false;
			}
		}
		else
		{
			Value = {};
			m_Indicator = 0;
		}

		return true;
	}

	bool Bind(CDBStatement& _Statement, SQLUSMALLINT _Index) override
	{
		return _Statement.BindParameter(
			_Index,
			GetOdbcParameterDirection(Direction),
			TOdbcScalarType<T>::CType,
			TOdbcScalarType<T>::SQLType,
			0,
			0,
			&Value,
			sizeof(T),
			&m_Indicator);
	}

private:
	SQLLEN m_Indicator = 0;
	bool m_IsAssigned = false;
};

template<size_t MaxLength, EDBParameterDirection Direction>
class TDBStringParameter final : public CDBParameter
{
public:
	string Value;

	TDBStringParameter& operator=(string _Value)
	{
		Value = move(_Value);
		m_IsAssigned = true;
		m_Indicator = SQL_NTS;
		return *this;
	}

	void SetNull()
	{
		m_IsAssigned = true;
		m_Indicator = SQL_NULL_DATA;
	}

	bool IsNull() const { return m_Indicator == SQL_NULL_DATA; }
	bool IsAssigned() const { return m_IsAssigned; }

	EDBParameterDirection GetDirection() const override
	{
		return Direction;
	}

	bool PrepareForBind() override
	{
		ClearError();
		m_WideBuffer.fill(L'\0');

		if constexpr (Direction == EDBParameterDirection::Input || Direction == EDBParameterDirection::InputOutput)
		{
			if (!m_IsAssigned)
			{
				SetError(L"A required string input parameter was not assigned");
				return false;
			}

			if (m_Indicator != SQL_NULL_DATA)
			{
				wstring WideValue;

				if (!Utf8ToWide(Value, WideValue))
				{
					SetError(L"The input string is not valid UTF-8");
					return false;
				}

				if (WideValue.size() > MaxLength)
				{
					SetError(L"The input string exceeds the SQL parameter length");
					return false;
				}

				copy(WideValue.begin(), WideValue.end(), m_WideBuffer.begin());
				m_Indicator = SQL_NTS;
			}
		}
		else
		{
			Value.clear();
			m_Indicator = 0;
		}

		return true;
	}

	bool Bind(CDBStatement& _Statement, SQLUSMALLINT _Index) override
	{
		return _Statement.BindParameter(
			_Index,
			GetOdbcParameterDirection(Direction),
			SQL_C_WCHAR,
			SQL_WVARCHAR,
			MaxLength,
			0,
			m_WideBuffer.data(),
			static_cast<SQLLEN>(m_WideBuffer.size() * sizeof(wchar_t)),
			&m_Indicator);
	}

	bool CompleteValue() override
	{
		if constexpr (Direction == EDBParameterDirection::Output || Direction == EDBParameterDirection::InputOutput || Direction == EDBParameterDirection::ReturnValue)
		{
			if (IsNull())
			{
				Value.clear();
				return true;
			}

			if (m_Indicator == SQL_NO_TOTAL || m_Indicator > static_cast<SQLLEN>(MaxLength * sizeof(wchar_t)))
			{
				SetError(L"The SQL output string was truncated");
				return false;
			}

			size_t Length = 0;

			while (Length < MaxLength && m_WideBuffer[Length] != L'\0')
			{
				++Length;
			}

			if (!WideToUtf8(wstring(m_WideBuffer.data(), Length), Value))
			{
				SetError(L"The SQL output string could not be converted to UTF-8");
				return false;
			}
		}

		return true;
	}

private:
	array<wchar_t, MaxLength + 1> m_WideBuffer{};
	SQLLEN m_Indicator = 0;
	bool m_IsAssigned = false;
};

template<typename T>
using TDBReturn = TDBScalarParameter<T, EDBParameterDirection::ReturnValue>;

template<typename T>
using TDBInput = TDBScalarParameter<T, EDBParameterDirection::Input>;

template<typename T>
using TDBOutput = TDBScalarParameter<T, EDBParameterDirection::Output>;

template<typename T>
using TDBInputOutput = TDBScalarParameter<T, EDBParameterDirection::InputOutput>;

template<size_t MaxLength>
using TDBInputString = TDBStringParameter<MaxLength, EDBParameterDirection::Input>;

template<size_t MaxLength>
using TDBOutputString = TDBStringParameter<MaxLength, EDBParameterDirection::Output>;

template<size_t MaxLength>
using TDBInputOutputString = TDBStringParameter<MaxLength, EDBParameterDirection::InputOutput>;

class CDBColumn
{
public:
	CDBColumn() = default;
	virtual ~CDBColumn() = default;

	CDBColumn(const CDBColumn&) = delete;
	CDBColumn& operator=(const CDBColumn&) = delete;
	CDBColumn(CDBColumn&&) noexcept = default;
	CDBColumn& operator=(CDBColumn&&) noexcept = default;

	virtual void PrepareForBind() = 0;
	virtual bool Bind(CDBStatement& _Statement, SQLUSMALLINT _Index) = 0;
	virtual bool CompleteValue() { return true; }

	const wstring& GetErrorMessage() const { return m_ErrorMessage; }

protected:
	void ClearError() { m_ErrorMessage.clear(); }
	void SetError(const wchar_t* _Message)
	{
		m_ErrorMessage = _Message == nullptr ? L"" : _Message;
	}

private:
	wstring m_ErrorMessage;
};

template<typename T>
class TDBColumn final : public CDBColumn
{
public:
	T Value{};

	bool IsNull() const { return m_Indicator == SQL_NULL_DATA; }

	void PrepareForBind() override
	{
		ClearError();
		Value = {};
		m_Indicator = 0;
	}

	bool Bind(CDBStatement& _Statement, SQLUSMALLINT _Index) override
	{
		return _Statement.BindColumn(_Index, TOdbcScalarType<T>::CType, &Value, sizeof(T), &m_Indicator);
	}

private:
	SQLLEN m_Indicator = 0;
};

template<size_t MaxLength>
class TDBStringColumn final : public CDBColumn
{
public:
	string Value;

	bool IsNull() const { return m_Indicator == SQL_NULL_DATA; }

	void PrepareForBind() override
	{
		ClearError();
		Value.clear();
		m_WideBuffer.fill(L'\0');
		m_Indicator = 0;
	}

	bool Bind(CDBStatement& _Statement, SQLUSMALLINT _Index) override
	{
		return _Statement.BindColumn(_Index, SQL_C_WCHAR, m_WideBuffer.data(), static_cast<SQLLEN>(m_WideBuffer.size() * sizeof(wchar_t)), &m_Indicator);
	}

	bool CompleteValue() override
	{
		if (IsNull())
		{
			Value.clear();
			return true;
		}

		if (m_Indicator == SQL_NO_TOTAL || m_Indicator > static_cast<SQLLEN>(MaxLength * sizeof(wchar_t)))
		{
			SetError(L"The SQL result string was truncated");
			return false;
		}

		size_t Length = 0;

		while (Length < MaxLength && m_WideBuffer[Length] != L'\0')
		{
			++Length;
		}

		if (!WideToUtf8(wstring(m_WideBuffer.data(), Length), Value))
		{
			SetError(L"The SQL result string could not be converted to UTF-8");
			return false;
		}

		return true;
	}

private:
	array<wchar_t, MaxLength + 1> m_WideBuffer{};
	SQLLEN m_Indicator = 0;
};
