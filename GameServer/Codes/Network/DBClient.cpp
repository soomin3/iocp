#include "pch.h"
#include "DBClient.h"
#include "DBTransport.h"

namespace
{
	PacketBuffer MakeFailureResponse(const PacketBuffer& _Request)
	{
		PacketHeader Header;
		memcpy(&Header, _Request.data(), sizeof(Header));
		if (Header.packetID == PacketID::SD_LOGIN_REQ)
		{
			SD_LOGIN_REQ Request;
			memcpy(&Request, _Request.data(), sizeof(Request));
			DS_LOGIN_RES Response;
			Response.sessionID = Request.sessionID;
			Response.generation = Request.generation;
			return MakePacketBuffer(Response);
		}
		SD_CREATE_NICKNAME_REQ Request;
		memcpy(&Request, _Request.data(), sizeof(Request));
		DS_CREATE_NICKNAME_RES Response;
		Response.requestID = Request.requestID;
		Response.accountID = Request.accountID;
		return MakePacketBuffer(Response);
	}

	bool MatchesRequest(const PacketBuffer& _Request, const PacketBuffer& _Response)
	{
		PacketHeader Header;
		memcpy(&Header, _Request.data(), sizeof(Header));
		if (Header.packetID == PacketID::SD_LOGIN_REQ && _Response.size() == sizeof(DS_LOGIN_RES))
		{
			SD_LOGIN_REQ Request;
			DS_LOGIN_RES Response;
			memcpy(&Request, _Request.data(), sizeof(Request));
			memcpy(&Response, _Response.data(), sizeof(Response));
			return Response.packetID == PacketID::DS_LOGIN_RES &&
				Response.sessionID == Request.sessionID && Response.generation == Request.generation;
		}
		if (Header.packetID == PacketID::SD_CREATE_NICKNAME_REQ && _Response.size() == sizeof(DS_CREATE_NICKNAME_RES))
		{
			SD_CREATE_NICKNAME_REQ Request;
			DS_CREATE_NICKNAME_RES Response;
			memcpy(&Request, _Request.data(), sizeof(Request));
			memcpy(&Response, _Response.data(), sizeof(Response));
			return Response.packetID == PacketID::DS_CREATE_NICKNAME_RES &&
				Response.requestID == Request.requestID && Response.accountID == Request.accountID;
		}
		return false;
	}
}

CDBClient::~CDBClient()
{
	StopWorker();
}

bool CDBClient::StartWorker(IPacketSender& _ResponseSender)
{
	if (m_Worker.joinable() || m_WsaStarted) return false;
	WSADATA WsaData{};
	if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0) return false;
	m_WsaStarted = true;
	m_StopRequested = false;
	m_ResponseSender = &_ResponseSender;
	m_NextConnectAttempt = 0;
	if (!m_Context.Open())
	{
		StopWorker();
		return false;
	}
	std::promise<bool> Started;
	std::future<bool> Ready = Started.get_future();
	try
	{
		m_Worker = thread(&CDBClient::WorkerThread, this, move(Started));
	}
	catch (const system_error&)
	{
		StopWorker();
		return false;
	}
	if (!Ready.get())
	{
		StopWorker();
		return false;
	}
	return true;
}

void CDBClient::StopWorker()
{
	{
		lock_guard<mutex> Lock(m_Lock);
		m_StopRequested = true;
	}
	if (m_Worker.joinable())
	{
		m_Context.Wake();
		m_Worker.join();
	}
	m_Context.Close();
	m_ResponseSender = nullptr;
	if (m_WsaStarted)
	{
		WSACleanup();
		m_WsaStarted = false;
	}
}

DBIocp::CTask CDBClient::Connect()
{
	if (IsStopping() || GetTickCount64() < m_NextConnectAttempt) co_return false;
	m_NextConnectAttempt = GetTickCount64() + 1000;
	m_Socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_Socket == INVALID_SOCKET) co_return false;
	sockaddr_in Address{};
	Address.sin_family = AF_INET;
	Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	Address.sin_port = htons(DBTransport::PORT);
	const bool Connected = DBTransport::ConfigureSocket(m_Socket) &&
		co_await DBIocp::Connect(m_Context, m_Socket, Address, GetTickCount64() + 1000);
	if (!Connected || IsStopping())
	{
		Disconnect();
		co_return false;
	}
	co_return true;
}

void CDBClient::Disconnect()
{
	// Socket lifetime belongs to the IOCP worker; cancellation is drained first.
	assert(m_Context.PendingCount() == 0);
	if (m_Socket != INVALID_SOCKET)
	{
		closesocket(m_Socket);
		m_Socket = INVALID_SOCKET;
	}
}

bool CDBClient::Submit(PacketBuffer _Request)
{
	if (_Request.size() < sizeof(PacketHeader)) return false;
	PacketHeader Header;
	memcpy(&Header, _Request.data(), sizeof(Header));
	if (Header.packetSize != _Request.size() || DBTransport::PacketSize(Header.packetID, true) != _Request.size()) return false;
	{
		lock_guard<mutex> Lock(m_Lock);
		if (m_StopRequested || m_Requests.size() >= MAX_JOB_QUEUE_SIZE) return false;
		// Publish the wake while holding the queue lock. The worker observes the
		// request only after it has been inserted, and a failed wake accepts nothing.
		if (m_RequestContinuation && !m_Context.Wake()) return false;
		m_Requests.push(move(_Request));
	}
	return true;
}

size_t CDBClient::GetPendingJobCount()
{
	lock_guard<mutex> Lock(m_Lock);
	return m_Requests.size();
}

uint64_t CDBClient::CreateRequestID()
{
	uint64_t RequestID = 0;
	do { RequestID = m_NextRequestID.fetch_add(1); } while (RequestID == 0);
	return RequestID;
}

bool CDBClient::IsStopping()
{
	lock_guard<mutex> Lock(m_Lock);
	return m_StopRequested;
}

bool CDBClient::SRequestAwaiter::await_suspend(std::coroutine_handle<> _Continuation) const
{
	lock_guard<mutex> Lock(Client.m_Lock);
	if (Client.m_StopRequested || !Client.m_Requests.empty()) return false;
	Client.m_RequestContinuation = _Continuation;
	return true;
}

void CDBClient::WorkerThread(std::promise<bool> _Started)
{
	m_Task = ProcessRequests(_Started);
	m_Task.Resume();
	while (!m_Task.IsDone())
	{
		if (IsStopping()) m_Context.CancelAll();
		m_Context.RunOne();
		std::coroutine_handle<> Continuation;
		{
			lock_guard<mutex> Lock(m_Lock);
			if (m_RequestContinuation && (m_StopRequested || !m_Requests.empty()))
			{
				Continuation = std::exchange(m_RequestContinuation, {});
			}
		}
		if (Continuation) Continuation.resume();
	}
	assert(m_Context.PendingCount() == 0);
	m_Task.Reset();
	Disconnect();
}

DBIocp::CTask CDBClient::ProcessRequests(std::promise<bool>& _Started)
{
	const bool Connected = co_await Connect();
	_Started.set_value(Connected);
	if (!Connected) co_return false;

	while (true)
	{
		co_await SRequestAwaiter{ *this };
		PacketBuffer Request;
		bool Stopping = false;
		{
			lock_guard<mutex> Lock(m_Lock);
			if (m_Requests.empty()) co_return true;
			Stopping = m_StopRequested;
			Request = move(m_Requests.front());
			m_Requests.pop();
		}
		PacketBuffer Response;
		if (Stopping || (m_Socket == INVALID_SOCKET && !co_await Connect()) ||
			!co_await DBIocp::SendPacket(m_Context, m_Socket, Request) ||
			!co_await DBIocp::ReceivePacket(m_Context, m_Socket, Response, false) || !MatchesRequest(Request, Response))
		{
			Disconnect();
			// Never replay an uncertain DB write after a transport failure.
			Response = MakeFailureResponse(Request);
		}
		m_ResponseSender->SendPacket(move(Response));
	}
}
