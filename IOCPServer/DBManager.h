#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <queue>
#include <string>
#include <thread>

#include "DBConnection.h"
#include "DBRequestHandlers.h"
#include "Packet.h"
#include "PacketSender.h"

using namespace std;

class CDBManager
{
public:
	CDBManager();
	~CDBManager();

	bool Init();
	void Release();

	bool Connect(const string& _ConfigFilePath, const string& _DatabaseName);
	void Disconnect();
	bool IsConnected() const { return m_Connection.IsConnected(); }

	bool StartWorker(IPacketSender& _ResponseSender);
	void StopWorker();
	size_t GetPendingJobCount();
	uint64_t CreateRequestID();

	bool Submit(PacketBuffer _Request);

private:
	bool Enqueue(PacketBuffer _Request);
	void WorkerThread();

private:
	CDBConnection m_Connection;

	CRITICAL_SECTION m_JobLock;
	CONDITION_VARIABLE m_JobCondition;
	queue<PacketBuffer> m_JobQueue;

	CDBLoginRequestHandler m_LoginRequestHandler;
	CDBCreateNicknameRequestHandler m_CreateNicknameRequestHandler;
	array<IDBRequestHandler*, static_cast<size_t>(PacketID::ID_MAX)> m_RequestHandlers{};

	thread m_WorkerThread;
	IPacketSender* m_ResponseSender;
	bool m_StopRequested;
	atomic_uint64_t m_NextRequestID;

	static constexpr size_t MAX_JOB_QUEUE_SIZE = 10000;
};
