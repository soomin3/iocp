#pragma once
#include <WinSock2.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include "PacketSender.h"

class CDBClient
{
public:
	~CDBClient();
	bool StartWorker(IPacketSender& _ResponseSender);
	void StopWorker();
	bool Submit(PacketBuffer _Request);
	size_t GetPendingJobCount();
	uint64_t CreateRequestID();

private:
	bool Connect();
	void Disconnect();
	void WorkerThread();

	std::mutex m_Lock;
	std::condition_variable m_Condition;
	std::queue<PacketBuffer> m_Requests;
	std::thread m_Worker;
	SOCKET m_Socket = INVALID_SOCKET;
	IPacketSender* m_ResponseSender = nullptr;
	bool m_StopRequested = true;
	bool m_WsaStarted = false;
	unsigned long long m_NextConnectAttempt = 0;
	std::atomic_uint64_t m_NextRequestID{1};
	static constexpr size_t MAX_JOB_QUEUE_SIZE = 10000;
};
