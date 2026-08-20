#include "pch.h"
#include "Session.h"
#include "Packet.h"
#include "PacketDispatcher.h"
#include "CriticalSectionLock.h"

extern LPFN_ACCEPTEX g_AcceptEx;

CSession::CSession(const CPacketDispatcher& _PacketDispatcher) : m_PacketDispatcher(_PacketDispatcher), m_Socket(INVALID_SOCKET), m_ReceivedBytes(0), m_PendingIOCount(0), m_State(ESessionState::DISCONNECTED), m_SendOffset(0), m_IsSending(false)
{
	InitializeCriticalSection(&m_StateLock);
	InitializeCriticalSection(&m_SendLock);
	InitializeCriticalSection(&m_PendingIOLock);

	InitializeConditionVariable(&m_PendingIOCCondition);

	//m_Socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

	memset(&m_AcceptOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_ReadOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_WriteOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	m_AcceptOverlapped.ioType = EIO_TYPE::ACCEPT;
	m_AcceptOverlapped.owner = this;

	m_ReadOverlapped.ioType = EIO_TYPE::READ;
	m_ReadOverlapped.owner = this;

	m_WriteOverlapped.ioType = EIO_TYPE::WRITE;
	m_WriteOverlapped.owner = this;

	memset(m_ReadBuffer, 0, sizeof(m_ReadBuffer));
}

CSession::~CSession()
{
	assert(m_PendingIOCount.load() == 0);
	DeleteCriticalSection(&m_StateLock);
	DeleteCriticalSection(&m_SendLock);
	DeleteCriticalSection(&m_PendingIOLock);
}

void CSession::Init()
{
}

void CSession::Disconnect()
{
	CCSLock Lock(&m_StateLock);

	if (m_State == ESessionState::DISCONNECTED || m_State == ESessionState::DISCONNECTING)
	{
		return;
	}

	m_State = ESessionState::DISCONNECTING;

	if (m_Socket != INVALID_SOCKET)
	{
		closesocket(m_Socket);
		m_Socket = INVALID_SOCKET;
	}
}

void CSession::Release()
{
	if (m_Socket != INVALID_SOCKET)
	{
		closesocket(m_Socket);
		m_Socket = INVALID_SOCKET;
	}
	memset(&m_AcceptOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_ReadOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_WriteOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	m_ReceivedBytes = 0;
}

bool CSession::TryReset()
{
	CCSLock StateLock(&m_StateLock);
	CCSLock SendLock(&m_SendLock);

	if (m_State != ESessionState::DISCONNECTING || m_PendingIOCount.load() != 0)
	{
		return false;
	}

	memset(&m_AcceptOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_ReadOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_WriteOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	m_ReceivedBytes = 0;

	m_SendQueue.clear();
	m_SendOffset = 0;
	m_IsSending = false;

	m_State = ESessionState::DISCONNECTED;

	return true;
}

bool CSession::PostAccept(SOCKET _ListenSocket)
{
	CCSLock Lock(&m_StateLock);

	if (m_State != ESessionState::DISCONNECTED)
	{
		return false;
	}

	m_State = ESessionState::ACCEPTING;

	m_Socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_Socket == INVALID_SOCKET)
	{
		m_State = ESessionState::DISCONNECTED;
		printf("WSASocket error: %d\n", WSAGetLastError());
		return false;
	}

	memset(&m_AcceptOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	m_AcceptOverlapped.ioType = EIO_TYPE::ACCEPT;
	m_AcceptOverlapped.owner = this;

	const int32_t AddrSize = sizeof(sockaddr_in) + 16;
	DWORD ReceiveSize = 0;

	m_PendingIOCount.fetch_add(1);
	bool Result = g_AcceptEx(_ListenSocket, m_Socket, m_AcceptBuffer, 0, AddrSize, AddrSize, &ReceiveSize, (LPOVERLAPPED)&m_AcceptOverlapped);

	if (Result == false)
	{
		if (WSAGetLastError() != WSA_IO_PENDING)
		{
			Release();
			m_State = ESessionState::DISCONNECTED;
			m_PendingIOCount.fetch_sub(1);
			//Release();
			return false;
		}
	}

	return true;
}

bool CSession::PostRead()
{
	CCSLock Lock(&m_StateLock);
	if (m_State != ESessionState::CONNECTED)
	{
		return false;
	}

	if (m_ReceivedBytes >= sizeof(m_ReadBuffer))
	{
		return false;
	}

	memset(&m_ReadOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	WSABUF wsaBuf;
	wsaBuf.buf = m_ReadBuffer + m_ReceivedBytes;
	wsaBuf.len = sizeof(m_ReadBuffer) - m_ReceivedBytes;

	DWORD bytesReceived = 0;
	DWORD flags = 0;

	m_PendingIOCount.fetch_add(1);
	int32_t result = WSARecv(m_Socket, &wsaBuf, 1, &bytesReceived, &flags, (LPWSAOVERLAPPED)&m_ReadOverlapped, nullptr);

	if (result == SOCKET_ERROR)
	{
		if (WSAGetLastError() != WSA_IO_PENDING) 
		{
			m_PendingIOCount.fetch_sub(1);
			return false;
		}
	}

	return true;
}

bool CSession::CompleteAccept()
{
	CCSLock Lock(&m_StateLock);

	if (m_State != ESessionState::ACCEPTING)
	{
		return false;
	}

	m_State = ESessionState::CONNECTED;

	return true;
}

bool CSession::Send(const void* _Data, size_t _Size)
{
	if (_Data == nullptr || _Size == 0)
	{
		return false;
	}

	bool StartWrite = false;

	//임계 영역 구분
	{
		CCSLock StateLock(&m_StateLock);
		if (m_State != ESessionState::CONNECTED)
		{
			return false;
		}

		CCSLock Lock(&m_SendLock);

		const char* begin = static_cast<const char*>(_Data);

		m_SendQueue.emplace_back(begin, begin + _Size);
		if (!m_IsSending)
		{
			m_IsSending = true;
			StartWrite = true;
		}
	}

	return !StartWrite || PostWrite();
}

bool CSession::PostWrite()
{
	CCSLock StateLock(&m_StateLock);

	if (m_State != ESessionState::CONNECTED)
	{
		return false;
	}

	CCSLock SendLock(&m_SendLock);

	if (m_SendQueue.empty())
	{
		return false;
	}

	vector<char>& Packet = m_SendQueue.front();
	memset(&m_WriteOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	WSABUF Buffer{};
	Buffer.buf = Packet.data() + m_SendOffset;
	Buffer.len = static_cast<ULONG>(Packet.size() - m_SendOffset);

	DWORD SentBytes = 0;

	m_PendingIOCount.fetch_add(1);

	const int32_t Result = WSASend(m_Socket, &Buffer, 1, &SentBytes, 0, &m_WriteOverlapped.Overlapped, nullptr);

	if (Result == SOCKET_ERROR)
	{
		const int32_t Error = WSAGetLastError();

		if (Error != WSA_IO_PENDING)
		{
			m_PendingIOCount.fetch_sub(1);
			return false;
		}
	}

	return true;
}

bool CSession::OnSend(DWORD _TransferredBytes)
{
	bool NextPostSend = false;

	// 임계 영역 구분
	{
		CCSLock Lock(&m_SendLock);

		if (m_SendQueue.empty())
		{
			return false;
		}

		vector<char>& Packet = m_SendQueue.front();
		const size_t Remaining = Packet.size() - m_SendOffset;

		if (_TransferredBytes == 0 || _TransferredBytes > Remaining)
		{
			return false;
		}

		m_SendOffset += _TransferredBytes;

		if (m_SendOffset == Packet.size())
		{
			m_SendQueue.pop_front();
			m_SendOffset = 0;
		}

		if (m_SendQueue.empty())
		{
			m_IsSending = false;
		}
		else
		{
			NextPostSend = true;
		}
	}

	return !NextPostSend || PostWrite();
}

bool CSession::OnRecv(DWORD _TransferredBytes)
{
	const size_t FreeBytes = sizeof(m_ReadBuffer) - m_ReceivedBytes;

	if (_TransferredBytes > FreeBytes)
	{
		return false;
	}

	m_ReceivedBytes += _TransferredBytes;

	if (m_ReceivedBytes < sizeof(PacketHeader))
	{
		return true;
	}

	size_t bytesProcessed = 0;
	while (m_ReceivedBytes - bytesProcessed >= sizeof(PacketHeader))
	{
		PacketHeader header = {};
		memcpy(&header, m_ReadBuffer + bytesProcessed, sizeof(PacketHeader));

		if (header.packetSize < sizeof(PacketHeader) || header.packetSize > sizeof(m_ReadBuffer))
		{
			return false;
		}

		if (m_ReceivedBytes - bytesProcessed < header.packetSize)
		{
			break;
		}

		if (!m_PacketDispatcher.Dispatch(*this, m_ReadBuffer + bytesProcessed, header))
		{
			return false;
		}

		bytesProcessed += header.packetSize;
	}

	const size_t remainingBytes = m_ReceivedBytes - bytesProcessed;
	memmove(m_ReadBuffer, m_ReadBuffer + bytesProcessed, remainingBytes);
	m_ReceivedBytes = static_cast<DWORD>(remainingBytes);

	return true;
}

int32_t CSession::CompleteIO()
{
	int32_t PreviousCount = m_PendingIOCount.fetch_sub(1);

	assert(PreviousCount > 0);

	const int32_t RemainingCount = PreviousCount - 1;
	if(RemainingCount == 0)
	{
		CCSLock Lock(&m_PendingIOLock);
		WakeAllConditionVariable(&m_PendingIOCCondition);
	}

	return PreviousCount - 1;
}

void CSession::WaitForPendingIO()
{
	CCSLock Lock(&m_PendingIOLock);
	while (m_PendingIOCount.load() != 0)
	{
		SleepConditionVariableCS(&m_PendingIOCCondition, &m_PendingIOLock, INFINITE);
	}
}
