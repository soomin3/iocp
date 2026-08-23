#include "pch.h"
#include "ChatLogManager.h"

#include <iomanip>
#include <sstream>
#include <system_error>

CChatLogManager::CChatLogManager() : m_StopRequested(true), m_Running(false), m_WriteHealthy(false), m_WrittenCount(0), m_FailureCount(0)
{
	InitializeCriticalSection(&m_LogLock);
	InitializeConditionVariable(&m_LogCondition);
}

CChatLogManager::~CChatLogManager()
{
	Stop();
	DeleteCriticalSection(&m_LogLock);
}

bool CChatLogManager::Start(const filesystem::path& _Directory)
{
	if (_Directory.empty())
	{
		return false;
	}

	error_code Error;
	filesystem::create_directories(_Directory, Error);
	if (Error)
	{
		cout << "Chat log directory creation failed: " << Error.message() << '\n';
		return false;
	}

	{
		CCSLock Lock(&m_LogLock);
		if (m_Running)
		{
			return false;
		}

		m_LogDirectory = _Directory;
		m_StopRequested = false;
		m_Running = true;
		m_WriteHealthy = true;
	}

	try
	{
		m_LogThread = thread(&CChatLogManager::WorkerThread, this);
	}
	catch (const system_error& ErrorException)
	{
		CCSLock Lock(&m_LogLock);
		m_StopRequested = true;
		m_Running = false;
		m_WriteHealthy = false;
		cout << "Chat log worker start failed: " << ErrorException.what() << '\n';
		return false;
	}

	return true;
}

void CChatLogManager::Stop()
{
	{
		CCSLock Lock(&m_LogLock);
		m_StopRequested = true;
		WakeAllConditionVariable(&m_LogCondition);
	}

	if (m_LogThread.joinable())
	{
		m_LogThread.join();
	}

	CCSLock Lock(&m_LogLock);
	m_Running = false;
}

bool CChatLogManager::Enqueue(SChatLogRecord _Record)
{
	CCSLock Lock(&m_LogLock);

	if (!m_Running || m_StopRequested || !m_WriteHealthy || m_LogQueue.size() >= MAX_LOG_QUEUE_SIZE)
	{
		return false;
	}

	m_LogQueue.push(move(_Record));
	WakeConditionVariable(&m_LogCondition);
	return true;
}

size_t CChatLogManager::GetQueueSize()
{
	CCSLock Lock(&m_LogLock);
	return m_LogQueue.size();
}

void CChatLogManager::WorkerThread()
{
	ofstream Stream;
	string OpenedDate;

	while (true)
	{
		SChatLogRecord Record;

		{
			CCSLock Lock(&m_LogLock);

			while (m_LogQueue.empty() && !m_StopRequested)
			{
				SleepConditionVariableCS(&m_LogCondition, &m_LogLock, INFINITE);
			}

			if (m_StopRequested && m_LogQueue.empty())
			{
				break;
			}

			Record = move(m_LogQueue.front());
			m_LogQueue.pop();
		}

		const string CurrentDate = GetUtcDate();
		if (!Stream.is_open() || OpenedDate != CurrentDate)
		{
			Stream.close();
			OpenedDate.clear();

			if (!OpenLogFile(Stream, OpenedDate))
			{
				m_FailureCount.fetch_add(1);
				CCSLock Lock(&m_LogLock);
				m_WriteHealthy = false;
				m_StopRequested = true;
				continue;
			}
		}

		if (!WriteRecord(Stream, Record))
		{
			m_FailureCount.fetch_add(1);
			CCSLock Lock(&m_LogLock);
			m_WriteHealthy = false;
			m_StopRequested = true;
			continue;
		}

		m_WrittenCount.fetch_add(1);
	}

	if (Stream.is_open())
	{
		Stream.flush();
		Stream.close();
	}
}

bool CChatLogManager::OpenLogFile(ofstream& _Stream, string& _OpenedDate)
{
	_OpenedDate = GetUtcDate();
	const filesystem::path FilePath = m_LogDirectory / ("chat-" + _OpenedDate + ".jsonl");
	_Stream.open(FilePath, ios::binary | ios::app);
	return _Stream.is_open();
}

bool CChatLogManager::WriteRecord(ofstream& _Stream, const SChatLogRecord& _Record)
{
	_Stream << "{\"utcMilliseconds\":" << _Record.UtcMilliseconds
		<< ",\"sequence\":" << _Record.Sequence
		<< ",\"accountId\":" << _Record.AccountID
		<< ",\"nickname\":\"" << EscapeJson(_Record.Nickname)
		<< "\",\"message\":\"" << EscapeJson(_Record.Message)
		<< "\"}\n";
	_Stream.flush();
	return _Stream.good();
}

string CChatLogManager::GetUtcDate()
{
	SYSTEMTIME Time{};
	GetSystemTime(&Time);

	ostringstream Builder;
	Builder << setfill('0')
		<< setw(4) << Time.wYear
		<< setw(2) << Time.wMonth
		<< setw(2) << Time.wDay;
	return Builder.str();
}

string CChatLogManager::EscapeJson(const string& _Value)
{
	ostringstream Builder;
	Builder << hex << uppercase;

	for (const unsigned char Character : _Value)
	{
		switch (Character)
		{
		case '"': Builder << "\\\""; break;
		case '\\': Builder << "\\\\"; break;
		case '\b': Builder << "\\b"; break;
		case '\f': Builder << "\\f"; break;
		case '\n': Builder << "\\n"; break;
		case '\r': Builder << "\\r"; break;
		case '\t': Builder << "\\t"; break;
		default:
			if (Character < 0x20)
			{
				Builder << "\\u" << setw(4) << setfill('0') << static_cast<int>(Character);
			}
			else
			{
				Builder << static_cast<char>(Character);
			}
			break;
		}
	}

	return Builder.str();
}
