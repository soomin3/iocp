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
	if (!Connect())
	{
		StopWorker();
		return false;
	}
	try
	{
		m_Worker = thread(&CDBClient::WorkerThread, this);
	}
	catch (const system_error&)
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
		// Only the worker closes the socket; shutdown interrupts its current I/O.
		if (m_Socket != INVALID_SOCKET) shutdown(m_Socket, SD_BOTH);
	}
	m_Condition.notify_all();
	if (m_Worker.joinable()) m_Worker.join();
	Disconnect();
	m_ResponseSender = nullptr;
	if (m_WsaStarted)
	{
		WSACleanup();
		m_WsaStarted = false;
	}
}

bool CDBClient::Connect()
{
	if (GetTickCount64() < m_NextConnectAttempt) return false;
	m_NextConnectAttempt = GetTickCount64() + 1000;
	SOCKET Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (Socket == INVALID_SOCKET) return false;
	{
		lock_guard<mutex> Lock(m_Lock);
		if (m_StopRequested)
		{
			closesocket(Socket);
			return false;
		}
		m_Socket = Socket;
	}
	u_long NonBlocking = 1;
	bool Connected = ioctlsocket(Socket, FIONBIO, &NonBlocking) == 0;
	sockaddr_in Address{};
	Address.sin_family = AF_INET;
	Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	Address.sin_port = htons(DBTransport::PORT);
	if (Connected && connect(Socket, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) != 0)
	{
		Connected = WSAGetLastError() == WSAEWOULDBLOCK;
		if (Connected)
		{
			fd_set WriteSet;
			fd_set ErrorSet;
			FD_ZERO(&WriteSet);
			FD_SET(Socket, &WriteSet);
			ErrorSet = WriteSet;
			timeval Timeout{1, 0};
			int Error = 0;
			int ErrorSize = sizeof(Error);
			Connected = select(0, nullptr, &WriteSet, &ErrorSet, &Timeout) > 0 &&
				FD_ISSET(Socket, &WriteSet) && !FD_ISSET(Socket, &ErrorSet) &&
				getsockopt(Socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&Error), &ErrorSize) == 0 && Error == 0;
		}
	}
	NonBlocking = 0;
	if (!Connected || ioctlsocket(Socket, FIONBIO, &NonBlocking) != 0 || !DBTransport::ConfigureSocket(Socket))
	{
		Disconnect();
		return false;
	}
	return true;
}

void CDBClient::Disconnect()
{
	lock_guard<mutex> Lock(m_Lock);
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
		m_Requests.push(move(_Request));
	}
	m_Condition.notify_one();
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

void CDBClient::WorkerThread()
{
	while (true)
	{
		PacketBuffer Request;
		bool Stopping = false;
		{
			unique_lock<mutex> Lock(m_Lock);
			m_Condition.wait(Lock, [this] { return m_StopRequested || !m_Requests.empty(); });
			if (m_Requests.empty()) return;
			Stopping = m_StopRequested;
			Request = move(m_Requests.front());
			m_Requests.pop();
		}
		PacketBuffer Response;
		if (Stopping || (m_Socket == INVALID_SOCKET && !Connect()) ||
			!DBTransport::SendPacket(m_Socket, Request) ||
			!DBTransport::ReceivePacket(m_Socket, Response, false) || !MatchesRequest(Request, Response))
		{
			Disconnect();
			// Never replay an uncertain DB write after a transport failure.
			Response = MakeFailureResponse(Request);
		}
		m_ResponseSender->SendPacket(move(Response));
	}
}
