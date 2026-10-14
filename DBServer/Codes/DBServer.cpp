#include <WinSock2.h>
#include <objbase.h>
#include "DBServer.h"

#include <cassert>
#include <iostream>
#include <utility>

CDBServer::~CDBServer()
{
	Stop();
	if (m_WsaStarted)
	{
		WSACleanup();
	}
}

bool CDBServer::Start(BackendFactory _Factory)
{
	if (m_WsaStarted)
	{
		return false;
	}
	WSADATA Data{};
	if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
	{
		return false;
	}
	m_WsaStarted = true;
	if (!m_Context.Open())
	{
		return false;
	}
	m_ListenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_ListenSocket == INVALID_SOCKET)
	{
		return false;
	}
	const BOOL Exclusive = TRUE;
	sockaddr_in Address{};
	Address.sin_family = AF_INET;
	Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	Address.sin_port = htons(DBTransport::PORT);
	if (setsockopt(m_ListenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
		reinterpret_cast<const char*>(&Exclusive), sizeof(Exclusive)) == SOCKET_ERROR ||
		::bind(m_ListenSocket, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == SOCKET_ERROR ||
		listen(m_ListenSocket, SOMAXCONN) == SOCKET_ERROR || !m_Context.Associate(m_ListenSocket))
	{
		return false;
	}
	std::promise<bool> Started;
	std::future<bool> Ready = Started.get_future();
	try
	{
		m_DatabaseWorker = std::thread(&CDBServer::WorkDatabase, this, std::move(_Factory), std::move(Started));
		if (!Ready.get())
		{
			Stop();
			return false;
		}
		m_Running.store(true);
		m_NetworkWorker = std::thread(&CDBServer::WorkNetwork, this);
	}
	catch (const std::system_error&)
	{
		Stop();
		return false;
	}
	return true;
}

void CDBServer::Stop()
{
	m_Running.store(false);
	m_Context.Wake();
	if (m_NetworkWorker.joinable())
	{
		m_NetworkWorker.join();
	}
	CloseSockets();
	// The network coroutine may still need a database result while draining shutdown.
	// Keep its worker alive until the coroutine and all socket completions have finished.
	{
		std::lock_guard<std::mutex> Lock(m_DatabaseLock);
		m_StopDatabase = true;
	}
	m_DatabaseReady.notify_one();
	if (m_DatabaseWorker.joinable())
	{
		m_DatabaseWorker.join();
	}
	m_Context.Close();
}

void CDBServer::PrintStatus() const
{
	std::cout << "running=" << (m_Running.load() ? "true" : "false")
		<< ", gameServerConnected=" << (m_Connected.load() ? "true" : "false")
		<< ", requests=" << m_Requests.load() << ", responses=" << m_Responses.load() << '\n';
}

void CDBServer::WorkNetwork()
{
	DBIocp::CTask Task = Serve();
	Task.Resume();
	bool Cancelling = false;
	while (!Task.IsDone() || m_Context.PendingCount() != 0)
	{
		if (!m_Running.load() && !Cancelling)
		{
			Cancelling = true;
			m_Context.CancelAll();
			CloseSockets();
		}
		m_Context.RunOne();
		CompleteDatabase();
	}
	Task.Reset();
	m_Running.store(false);
	CloseSockets();
}

DBIocp::CTask CDBServer::Serve()
{
	while (m_Running.load())
	{
		m_ClientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (m_ClientSocket == INVALID_SOCKET)
		{
			break;
		}
		if (!(co_await DBIocp::Accept(m_Context, m_ListenSocket, m_ClientSocket)))
		{
			CloseClient();
			continue;
		}
		const BOOL NoDelay = TRUE;
		if (!m_Running.load() || setsockopt(m_ClientSocket, IPPROTO_TCP, TCP_NODELAY,
			reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay)) == SOCKET_ERROR)
		{
			CloseClient();
			continue;
		}
		m_Connected.store(true);
		std::cout << "GameServer connected\n";
		while (m_Running.load())
		{
			PacketBuffer Request;
			if (!(co_await DBIocp::ReceivePacket(m_Context, m_ClientSocket, Request, true, true)) || !m_Running.load())
			{
				break;
			}
			++m_Requests;
			PacketBuffer Response = co_await SDatabaseAwaiter{ *this, Request };
			if (!m_Running.load() || Response.empty() || !(co_await DBIocp::SendPacket(m_Context, m_ClientSocket, Response)))
			{
				break;
			}
			++m_Responses;
		}
		CloseClient();
		std::cout << "GameServer disconnected\n";
	}
	co_return true;
}

void CDBServer::WorkDatabase(BackendFactory _Factory, std::promise<bool> _Started)
{
	const HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(Result))
	{
		_Started.set_value(false);
		return;
	}
	{
		RequestHandler Handler = _Factory();
		_Started.set_value(static_cast<bool>(Handler));
		if (Handler)
		{
			for (;;)
			{
				PacketBuffer Request;
				{
					std::unique_lock<std::mutex> Lock(m_DatabaseLock);
					m_DatabaseReady.wait(Lock, [this] { return m_StopDatabase || m_RequestReady; });
					if (!m_RequestReady)
					{
						break;
					}
					Request = std::move(m_Request);
					m_RequestReady = false;
				}
				PacketBuffer Response = Handler(Request);
				{
					std::lock_guard<std::mutex> Lock(m_DatabaseLock);
					m_Response = std::move(Response);
					m_ResponseReady = true;
				}
				m_Context.Wake();
			}
		}
	}
	CoUninitialize();
}

void CDBServer::SDatabaseAwaiter::await_suspend(std::coroutine_handle<> _Continuation) const
{
	{
		std::lock_guard<std::mutex> Lock(Server.m_DatabaseLock);
		assert(!Server.m_DatabaseContinuation && !Server.m_RequestReady && !Server.m_ResponseReady);
		Server.m_DatabaseContinuation = _Continuation;
		Server.m_Request = std::move(Request);
		Server.m_RequestReady = true;
	}
	Server.m_DatabaseReady.notify_one();
}

PacketBuffer CDBServer::SDatabaseAwaiter::await_resume() const
{
	std::lock_guard<std::mutex> Lock(Server.m_DatabaseLock);
	return std::move(Server.m_Response);
}

void CDBServer::CompleteDatabase()
{
	std::coroutine_handle<> Continuation;
	{
		std::lock_guard<std::mutex> Lock(m_DatabaseLock);
		if (!m_ResponseReady)
		{
			return;
		}
		m_ResponseReady = false;
		Continuation = std::exchange(m_DatabaseContinuation, {});
	}
	assert(Continuation);
	Continuation.resume();
}

void CDBServer::CloseClient()
{
	if (m_ClientSocket != INVALID_SOCKET)
	{
		closesocket(m_ClientSocket);
		m_ClientSocket = INVALID_SOCKET;
	}
	m_Connected.store(false);
}

void CDBServer::CloseSockets()
{
	CloseClient();
	if (m_ListenSocket != INVALID_SOCKET)
	{
		closesocket(m_ListenSocket);
		m_ListenSocket = INVALID_SOCKET;
	}
}
