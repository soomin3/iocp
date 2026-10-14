#pragma once

#include "DBIocp.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

class CDBServer
{
public:
	using RequestHandler = std::function<PacketBuffer(const PacketBuffer&)>;
	using BackendFactory = std::function<RequestHandler()>;

	~CDBServer();
	// The factory and every request execute on the dedicated COM worker.
	bool Start(BackendFactory _Factory);
	void Stop();
	void PrintStatus() const;
	bool IsRunning() const { return m_Running.load(); }

private:
	struct SDatabaseAwaiter
	{
		CDBServer& Server;
		PacketBuffer& Request;
		bool await_ready() const noexcept { return false; }
		void await_suspend(std::coroutine_handle<> _Continuation) const;
		PacketBuffer await_resume() const;
	};

	void WorkNetwork();
	void WorkDatabase(BackendFactory _Factory, std::promise<bool> _Started);
	void CompleteDatabase();
	void CloseClient();
	void CloseSockets();
	DBIocp::CTask Serve();

	DBIocp::CContext m_Context;
	SOCKET m_ListenSocket = INVALID_SOCKET;
	SOCKET m_ClientSocket = INVALID_SOCKET;
	bool m_WsaStarted = false;
	std::atomic_bool m_Running{ false };
	std::atomic_bool m_Connected{ false };
	std::atomic_uint64_t m_Requests{ 0 };
	std::atomic_uint64_t m_Responses{ 0 };
	std::thread m_NetworkWorker;
	std::thread m_DatabaseWorker;
	std::mutex m_DatabaseLock;
	std::condition_variable m_DatabaseReady;
	bool m_StopDatabase = false;
	bool m_RequestReady = false;
	bool m_ResponseReady = false;
	PacketBuffer m_Request;
	PacketBuffer m_Response;
	std::coroutine_handle<> m_DatabaseContinuation;
};
