#include "pch.h"
#include "DBManager.h"

#include <array>
#include <charconv>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include "TextEncoding.h"

namespace
{
	constexpr wchar_t DB_DRIVER[] = L"ODBC Driver 18 for SQL Server";
	constexpr wchar_t DB_SECURITY_OPTIONS[] = L"Encrypt=yes;TrustServerCertificate=yes;";

	string TrimConfigField(string _Value)
	{
		const size_t Begin = _Value.find_first_not_of(" \t\r\n");
		if (Begin == string::npos)
		{
			return {};
		}

		const size_t End = _Value.find_last_not_of(" \t\r\n");
		return _Value.substr(Begin, End - Begin + 1);
	}

	bool ParseConfigValue(const string& _Value, array<string, 4>& _Fields)
	{
		size_t FieldBegin = 0;
		for (size_t Index = 0; Index < _Fields.size() - 1; ++Index)
		{
			const size_t Separator = _Value.find(',', FieldBegin);
			if (Separator == string::npos)
			{
				return false;
			}

			_Fields[Index] = TrimConfigField(_Value.substr(FieldBegin, Separator - FieldBegin));
			FieldBegin = Separator + 1;
		}

		_Fields.back() = TrimConfigField(_Value.substr(FieldBegin));

		if (_Fields[0].empty() || _Fields[1].empty() ||
			_Fields[2].empty() != _Fields[3].empty())
		{
			return false;
		}

		uint32_t Port = 0;
		const char* PortBegin = _Fields[1].data();
		const char* PortEnd = PortBegin + _Fields[1].size();
		const auto [ParseEnd, Error] = from_chars(PortBegin, PortEnd, Port);
		return Error == errc{} && ParseEnd == PortEnd && Port > 0 && Port <= 65535;
	}

	wstring EscapeOdbcValue(const wstring& _Value)
	{
		wstring Escaped = L"{";
		Escaped.reserve(_Value.size() + 2);

		for (const wchar_t Character : _Value)
		{
			Escaped.push_back(Character);
			if (Character == L'}')
			{
				Escaped.push_back(L'}');
			}
		}

		Escaped.push_back(L'}');
		return Escaped;
	}

}

CDBManager::CDBManager(): m_ResponseSender(nullptr), m_StopRequested(true), m_NextRequestID(1)
{
	InitializeCriticalSection(&m_JobLock);
	InitializeConditionVariable(&m_JobCondition);

	m_RequestHandlers[static_cast<size_t>(PacketID::SD_LOGIN_REQ)] = &m_LoginRequestHandler;
	m_RequestHandlers[static_cast<size_t>(PacketID::SD_CREATE_NICKNAME_REQ)] = &m_CreateNicknameRequestHandler;
}

CDBManager::~CDBManager()
{
	Release();
	DeleteCriticalSection(&m_JobLock);
}

bool CDBManager::Init()
{
	return m_Connection.Init();
}

void CDBManager::Release()
{
	StopWorker();
	m_Connection.Release();
}

bool CDBManager::Connect(const string& _ConfigFilePath, const string& _DatabaseName)
{
	if (_DatabaseName.empty())
	{
		cout << "DB name is empty\n";
		return false;
	}

	ifstream ConfigFile(_ConfigFilePath);

	if (!ConfigFile.is_open())
	{
		cout << "DB config file open failed: " << _ConfigFilePath << '\n';
		return false;
	}

	string SelectedConfigValue;
	unordered_set<string> DatabaseNames;
	string Line;
	size_t LineNumber = 0;

	while (getline(ConfigFile, Line))
	{
		++LineNumber;

		const size_t LineBegin = Line.find_first_not_of(" \t\r\n");

		if (LineBegin == string::npos || Line[LineBegin] == ';')
		{
			continue;
		}

		const size_t Separator = Line.find('=', LineBegin);

		if (Separator == string::npos || Separator == LineBegin)
		{
			cout << "Invalid DB config line: " << LineNumber << '\n';
			return false;
		}

		const size_t NameEnd = Line.find_last_not_of(" \t", Separator - 1);
		const size_t ValueBegin = Line.find_first_not_of(" \t", Separator + 1);
		const size_t ValueEnd = Line.find_last_not_of(" \t\r\n");

		if (NameEnd == string::npos || ValueBegin == string::npos || ValueEnd < ValueBegin)
		{
			cout << "Invalid DB config value: " << LineNumber << '\n';
			return false;
		}

		const string DatabaseName = Line.substr(LineBegin, NameEnd - LineBegin + 1);
		if (!DatabaseNames.insert(DatabaseName).second)
		{
			cout << "Duplicate DB name: " << DatabaseName << '\n';
			return false;
		}

		if (DatabaseName == _DatabaseName)
		{
			SelectedConfigValue = Line.substr(ValueBegin, ValueEnd - ValueBegin + 1);
		}
	}

	if (SelectedConfigValue.empty())
	{
		cout << "DB was not found: " << _DatabaseName << '\n';
		return false;
	}

	array<string, 4> Fields;
	if (!ParseConfigValue(SelectedConfigValue, Fields))
	{
		cout << "Invalid DB config fields: " << _DatabaseName << '\n';
		return false;
	}

	wstring DatabaseName;
	if (!Utf8ToWide(_DatabaseName, DatabaseName))
	{
		cout << "DB name contains invalid UTF-8\n";
		return false;
	}

	array<wstring, 4> WideFields;
	for (size_t Index = 0; Index < Fields.size(); ++Index)
	{
		if (!Utf8ToWide(Fields[Index], WideFields[Index]))
		{
			cout << "DB config contains invalid UTF-8\n";
			return false;
		}
	}

	const wstring Server = WideFields[0] + L"," + WideFields[1];
	wstring ConnectionString = L"Driver={" + wstring(DB_DRIVER) + L"};Server=" +
		EscapeOdbcValue(Server) + L";Database=" + EscapeOdbcValue(DatabaseName) +
		L";" + DB_SECURITY_OPTIONS;

	if (WideFields[2].empty())
	{
		ConnectionString += L"Trusted_Connection=yes;";
	}
	else
	{
		ConnectionString += L"UID=" + EscapeOdbcValue(WideFields[2]) +
			L";PWD=" + EscapeOdbcValue(WideFields[3]) + L";";
	}

	return m_Connection.Connect(ConnectionString);
}

void CDBManager::Disconnect()
{
	StopWorker();
	m_Connection.Disconnect();
}

bool CDBManager::StartWorker(IPacketSender& _ResponseSender)
{
	if (!m_Connection.IsConnected() || m_WorkerThread.joinable())
	{
		return false;
	}

	m_ResponseSender = &_ResponseSender;

	{
		CCSLock Lock(&m_JobLock);
		m_StopRequested = false;
	}

	try
	{
		m_WorkerThread = thread(&CDBManager::WorkerThread, this);
	}
	catch (const system_error& Error)
	{
		CCSLock Lock(&m_JobLock);
		m_StopRequested = true;
		cout << "DB worker start failed: " << Error.what() << '\n';
		return false;
	}

	return true;
}

void CDBManager::StopWorker()
{
	{
		CCSLock Lock(&m_JobLock);
		m_StopRequested = true;
		WakeAllConditionVariable(&m_JobCondition);
	}

	if (m_WorkerThread.joinable())
	{
		m_WorkerThread.join();
	}
}

size_t CDBManager::GetPendingJobCount()
{
	CCSLock Lock(&m_JobLock);
	return m_JobQueue.size();
}

uint64_t CDBManager::CreateRequestID()
{
	while (true)
	{
		const uint64_t RequestID = m_NextRequestID.fetch_add(1);
		if (RequestID != 0)
		{
			return RequestID;
		}
	}
}

bool CDBManager::Submit(PacketBuffer _Request)
{
	return Enqueue(move(_Request));
}

bool CDBManager::Enqueue(PacketBuffer _Request)
{
	if (_Request.size() < sizeof(PacketHeader) || !m_Connection.IsConnected() || m_ResponseSender == nullptr)
	{
		return false;
	}

	PacketHeader Header;
	memcpy(&Header, _Request.data(), sizeof(Header));

	const size_t HandlerIndex = static_cast<size_t>(Header.packetID);
	if (Header.packetSize != _Request.size() || HandlerIndex >= m_RequestHandlers.size() ||
		m_RequestHandlers[HandlerIndex] == nullptr)
	{
		return false;
	}

	{
		CCSLock Lock(&m_JobLock);

		if (m_StopRequested || m_JobQueue.size() >= MAX_JOB_QUEUE_SIZE)
		{
			return false;
		}

		m_JobQueue.push(move(_Request));
		WakeConditionVariable(&m_JobCondition);
	}

	return true;
}

void CDBManager::WorkerThread()
{
	while (true)
	{
		PacketBuffer Request;

		{
			CCSLock Lock(&m_JobLock);

			while (m_JobQueue.empty() && !m_StopRequested)
			{
				SleepConditionVariableCS(&m_JobCondition, &m_JobLock, INFINITE);
			}

			if (m_StopRequested && m_JobQueue.empty())
			{
				return;
			}

			Request = move(m_JobQueue.front());
			m_JobQueue.pop();
		}

		if (Request.size() < sizeof(PacketHeader) || m_ResponseSender == nullptr)
		{
			continue;
		}

		PacketHeader Header;
		memcpy(&Header, Request.data(), sizeof(Header));

		const size_t HandlerIndex = static_cast<size_t>(Header.packetID);
		if (Header.packetSize != Request.size() || HandlerIndex >= m_RequestHandlers.size())
		{
			continue;
		}

		IDBRequestHandler* Handler = m_RequestHandlers[HandlerIndex];
		if (Handler == nullptr)
		{
			continue;
		}

		if (!Handler->Handle(m_Connection, *m_ResponseSender, Request.data(), Header))
		{
			cout << "DB request handling failed: " << static_cast<uint16_t>(Header.packetID) << '\n';
		}
	}
}
