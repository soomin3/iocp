#include "OleDbDatabase.h"

#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{
	struct SLoginParameters
	{
		LONG Result = -1;
		DBSTATUS ResultStatus = DBSTATUS_E_UNAVAILABLE;
		WCHAR ID[65]{};
		DBLENGTH IDLength = 0;
		DBSTATUS IDStatus = DBSTATUS_S_OK;
		WCHAR PW[65]{};
		DBLENGTH PWLength = 0;
		DBSTATUS PWStatus = DBSTATUS_S_OK;
		LONGLONG AccountID = 0;
		DBSTATUS AccountStatus = DBSTATUS_E_UNAVAILABLE;
		WCHAR Nickname[17]{};
		DBLENGTH NicknameLength = 0;
		DBSTATUS NicknameStatus = DBSTATUS_E_UNAVAILABLE;

		BEGIN_PARAM_MAP(SLoginParameters)
			SET_PARAM_TYPE(DBPARAMIO_OUTPUT)
			COLUMN_ENTRY_STATUS(1, Result, ResultStatus)
			SET_PARAM_TYPE(DBPARAMIO_INPUT)
			COLUMN_ENTRY_LENGTH_STATUS(2, ID, IDLength, IDStatus)
			COLUMN_ENTRY_LENGTH_STATUS(3, PW, PWLength, PWStatus)
			SET_PARAM_TYPE(DBPARAMIO_OUTPUT)
			COLUMN_ENTRY_STATUS(4, AccountID, AccountStatus)
			COLUMN_ENTRY_LENGTH_STATUS(5, Nickname, NicknameLength, NicknameStatus)
		END_PARAM_MAP()
	};

	struct SNicknameParameters
	{
		LONG Result = -1;
		DBSTATUS ResultStatus = DBSTATUS_E_UNAVAILABLE;
		LONGLONG AccountID = 0;
		WCHAR Nickname[17]{};
		DBLENGTH NicknameLength = 0;
		DBSTATUS NicknameStatus = DBSTATUS_S_OK;

		BEGIN_PARAM_MAP(SNicknameParameters)
			SET_PARAM_TYPE(DBPARAMIO_OUTPUT)
			COLUMN_ENTRY_STATUS(1, Result, ResultStatus)
			SET_PARAM_TYPE(DBPARAMIO_INPUT)
			COLUMN_ENTRY(2, AccountID)
			COLUMN_ENTRY_LENGTH_STATUS(3, Nickname, NicknameLength, NicknameStatus)
		END_PARAM_MAP()
	};

	template<typename Parameters>
	using TProcedure = ATL::CCommand<ATL::CAccessor<Parameters>, ATL::CRowset, ATL::CMultipleResults>;

	template<typename Parameters>
	bool Execute(TProcedure<Parameters>& _Command, ATL::CSession& _Session, const wchar_t* _SQL)
	{
		ATL::CDBPropSet Properties(DBPROPSET_ROWSET);
		if (!Properties.AddProperty(DBPROP_COMMANDTIMEOUT, 3L, DBPROPOPTIONS_REQUIRED))
		{
			return false;
		}
		HRESULT Result = _Command.Create(_Session, _SQL);
		ATL::CComPtr<ICommandProperties> CommandProperties;
		if (SUCCEEDED(Result))
		{
			Result = _Command.m_spCommand->QueryInterface(&CommandProperties);
		}
		if (SUCCEEDED(Result))
		{
			Result = CommandProperties->SetProperties(1, &Properties);
		}
		DBPARAMS ParametersBuffer{};
		if (SUCCEEDED(Result))
		{
			Result = _Command.BindParameters(&_Command.m_hParameterAccessor,
				_Command.m_spCommand, &ParametersBuffer.pData);
		}
		if (SUCCEEDED(Result))
		{
			ParametersBuffer.cParamSets = 1;
			ParametersBuffer.hAccessor = _Command.m_hParameterAccessor;
			// ATL CCommand::Open retries failed IMultipleResults execution as IRowset.
			// Execute directly once: a timed-out write must never be replayed.
			Result = _Command.m_spCommand->Execute(nullptr, __uuidof(IMultipleResults),
				&ParametersBuffer, nullptr, reinterpret_cast<IUnknown**>(_Command.GetMultiplePtrAddress()));
		}
		while (SUCCEEDED(Result) && Result != DB_S_NORESULT && _Command.GetMultiplePtr() != nullptr)
		{
			DBROWCOUNT Rows = 0;
			Result = _Command.GetNextResult(&Rows, false);
		}
		// SQL Server publishes output parameters after all result sets are released.
		if (Result != DB_S_NORESULT || _Command.ResultStatus != DBSTATUS_S_OK)
		{
			std::cout << "Stored procedure failed (HRESULT=" << std::hex
				<< static_cast<unsigned long>(Result) << std::dec << ")\n";
			return false;
		}
		return true;
	}

	bool ToWide(std::string_view _Value, std::wstring& _Output)
	{
		if (_Value.empty())
		{
			_Output.clear();
			return true;
		}
		if (_Value.size() > static_cast<size_t>(INT_MAX))
		{
			return false;
		}
		const int Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			_Value.data(), static_cast<int>(_Value.size()), nullptr, 0);
		if (Length <= 0)
		{
			return false;
		}
		_Output.resize(static_cast<size_t>(Length));
		return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _Value.data(),
			static_cast<int>(_Value.size()), _Output.data(), Length) == Length;
	}

	template<size_t InputSize, size_t OutputSize>
	bool ReadString(const char (&_Input)[InputSize], wchar_t (&_Output)[OutputSize], DBLENGTH& _Length)
	{
		const size_t Length = strnlen_s(_Input, InputSize);
		std::wstring Value;
		if (Length == 0 || Length == InputSize || !ToWide(std::string_view(_Input, Length), Value) ||
			Value.size() >= OutputSize)
		{
			return false;
		}
		_Length = Value.size() * sizeof(wchar_t);
		memcpy(_Output, Value.data(), static_cast<size_t>(_Length));
		return true;
	}

	std::string Trim(const std::string& _Value)
	{
		const size_t Begin = _Value.find_first_not_of(" \t\r\n");
		return Begin == std::string::npos ? std::string{} :
			_Value.substr(Begin, _Value.find_last_not_of(" \t\r\n") - Begin + 1);
	}

	bool ReadConfig(std::array<std::wstring, 4>& _Fields)
	{
		std::ifstream File("DBConfig.txt");
		if (!File)
		{
			return false;
		}
		std::unordered_set<std::string> Names;
		std::string Selected;
		std::string Line;
		while (getline(File, Line))
		{
			Line = Trim(Line);
			if (Line.empty() || Line[0] == ';')
			{
				continue;
			}
			const size_t Separator = Line.find('=');
			if (Separator == std::string::npos)
			{
				return false;
			}
			const std::string Name = Trim(Line.substr(0, Separator));
			const std::string Value = Trim(Line.substr(Separator + 1));
			if (Name.empty() || Value.empty() || !Names.insert(Name).second)
			{
				return false;
			}
			if (Name == "TestDB")
			{
				Selected = Value;
			}
		}
		std::array<std::string, 4> Fields;
		size_t Begin = 0;
		for (size_t Index = 0; Index < Fields.size() - 1; ++Index)
		{
			const size_t Separator = Selected.find(',', Begin);
			if (Separator == std::string::npos)
			{
				return false;
			}
			Fields[Index] = Trim(Selected.substr(Begin, Separator - Begin));
			Begin = Separator + 1;
		}
		Fields.back() = Trim(Selected.substr(Begin));
		unsigned int Port = 0;
		const auto [End, Error] = std::from_chars(Fields[1].data(), Fields[1].data() + Fields[1].size(), Port);
		if (Fields[0].empty() || Error != std::errc{} || End != Fields[1].data() + Fields[1].size() ||
			Port == 0 || Port > 65535 || Fields[2].empty() != Fields[3].empty())
		{
			return false;
		}
		for (size_t Index = 0; Index < Fields.size(); ++Index)
		{
			if (Fields[Index].find('\0') != std::string::npos || !ToWide(Fields[Index], _Fields[Index]))
			{
				return false;
			}
		}
		return true;
	}

	std::wstring QuoteValue(const std::wstring& _Value)
	{
		std::wstring Result = L"\"";
		for (const wchar_t Character : _Value)
		{
			Result += Character;
			if (Character == L'"')
			{
				Result += Character;
			}
		}
		return Result + L"\"";
	}
}

bool COleDbDatabase::Open()
{
	std::array<std::wstring, 4> Fields;
	if (!ReadConfig(Fields))
	{
		std::cout << "DBConfig.txt is missing or invalid (TestDB required)\n";
		return false;
	}
	std::wstring Connection = L"Provider=MSOLEDBSQL19;Data Source=" + QuoteValue(Fields[0] + L"," + Fields[1]) +
		L";Initial Catalog=TestDB;Use Encryption for Data=Mandatory;Trust Server Certificate=True;"
		L"Persist Security Info=False;Connect Timeout=3;Connect Retry Count=0;";
	if (Fields[2].empty())
	{
		Connection += L"Integrated Security=SSPI;";
	}
	else
	{
		Connection += L"User ID=" + QuoteValue(Fields[2]) + L";Password=" + QuoteValue(Fields[3]) + L";";
	}
	HRESULT Result = m_DataSource.OpenFromInitializationString(Connection.c_str());
	if (SUCCEEDED(Result))
	{
		Result = m_Session.Open(m_DataSource);
	}
	if (FAILED(Result))
	{
		std::cout << "OLE DB connection failed (HRESULT=" << std::hex
			<< static_cast<unsigned long>(Result) << std::dec << ")\n";
		return false;
	}
	return true;
}

PacketBuffer COleDbDatabase::Handle(const PacketBuffer& _Request)
{
	if (_Request.size() < sizeof(PacketHeader))
	{
		return {};
	}
	PacketHeader Header;
	memcpy(&Header, _Request.data(), sizeof(Header));
	if (Header.packetSize != _Request.size())
	{
		return {};
	}
	if (Header.packetID == PacketID::SD_LOGIN_REQ && _Request.size() == sizeof(SD_LOGIN_REQ))
	{
		SD_LOGIN_REQ Request;
		memcpy(&Request, _Request.data(), sizeof(Request));
		return Login(Request);
	}
	if (Header.packetID == PacketID::SD_CREATE_NICKNAME_REQ && _Request.size() == sizeof(SD_CREATE_NICKNAME_REQ))
	{
		SD_CREATE_NICKNAME_REQ Request;
		memcpy(&Request, _Request.data(), sizeof(Request));
		return CreateNickname(Request);
	}
	return {};
}

PacketBuffer COleDbDatabase::Login(const SD_LOGIN_REQ& _Request)
{
	DS_LOGIN_RES Response;
	Response.sessionID = _Request.sessionID;
	Response.generation = _Request.generation;
	TProcedure<SLoginParameters> Command;
	if (!ReadString(_Request.id, Command.ID, Command.IDLength) ||
		!ReadString(_Request.pw, Command.PW, Command.PWLength) ||
		!Execute(Command, m_Session, L"{? = CALL dbo.tp_UserLogin(?, ?, ?, ?)}"))
	{
		return MakePacketBuffer(Response);
	}
	if (Command.Result == 0)
	{
		if (Command.AccountStatus != DBSTATUS_S_OK ||
			(Command.NicknameStatus != DBSTATUS_S_OK && Command.NicknameStatus != DBSTATUS_S_ISNULL))
		{
			return MakePacketBuffer(Response);
		}
		Response.accountID = Command.AccountID;
		Response.hasAccountID = 1;
		if (Command.NicknameStatus == DBSTATUS_S_OK && Command.NicknameLength > 0)
		{
			if (Command.NicknameLength > 16 * sizeof(wchar_t) || Command.NicknameLength % sizeof(wchar_t) != 0)
			{
				return MakePacketBuffer(Response);
			}
			const int Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Command.Nickname,
				static_cast<int>(Command.NicknameLength / sizeof(wchar_t)), Response.nickname,
				static_cast<int>(sizeof(Response.nickname) - 1), nullptr, nullptr);
			if (Length <= 0)
			{
				return MakePacketBuffer(Response);
			}
			Response.hasNickname = 1;
		}
	}
	Response.resultCode = Command.Result;
	Response.dbSucceeded = 1;
	return MakePacketBuffer(Response);
}

PacketBuffer COleDbDatabase::CreateNickname(const SD_CREATE_NICKNAME_REQ& _Request)
{
	DS_CREATE_NICKNAME_RES Response;
	Response.requestID = _Request.requestID;
	Response.accountID = _Request.accountID;
	TProcedure<SNicknameParameters> Command;
	Command.AccountID = _Request.accountID;
	if (_Request.accountID <= 0 || !ReadString(_Request.nickname, Command.Nickname, Command.NicknameLength))
	{
		return MakePacketBuffer(Response);
	}
	memcpy(Response.nickname, _Request.nickname, sizeof(Response.nickname));
	if (Execute(Command, m_Session, L"{? = CALL dbo.tp_UserCreateNickname(?, ?)}"))
	{
		Response.resultCode = Command.Result;
		Response.dbSucceeded = 1;
	}
	return MakePacketBuffer(Response);
}
