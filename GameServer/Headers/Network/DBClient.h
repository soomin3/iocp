#pragma once
#include <WinSock2.h>
#include <atomic>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include "PacketSender.h"
#include "DBIocp.h"

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
	struct SRequestAwaiter
	{
		CDBClient& Client;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<> _Continuation) const;
		void await_resume() const noexcept {}
	};

	DBIocp::CTask Connect();
	DBIocp::CTask ProcessRequests(std::promise<bool>& _Started);
	bool IsStopping();
	void Disconnect();
	void WorkerThread(std::promise<bool> _Started);

	std::mutex m_Lock;
	std::queue<PacketBuffer> m_Requests;
	std::thread m_Worker;
	DBIocp::CContext m_Context;
	DBIocp::CTask m_Task;
	std::coroutine_handle<> m_RequestContinuation{};
	SOCKET m_Socket = INVALID_SOCKET;
	IPacketSender* m_ResponseSender = nullptr;
	bool m_StopRequested = true;
	bool m_WsaStarted = false;
	unsigned long long m_NextConnectAttempt = 0;
	std::atomic_uint64_t m_NextRequestID{1};
	static constexpr size_t MAX_JOB_QUEUE_SIZE = 10000;
};
