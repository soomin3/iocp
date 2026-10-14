#include "pch.h"
#include "Session.h"
#include "Packet.h"
#include "PacketDispatcher.h"

extern LPFN_ACCEPTEX g_AcceptEx;

CSession::CSession(const CPacketDispatcher& _PacketDispatcher, uint32_t _SessionID) :
	m_Socket(INVALID_SOCKET),
	m_SessionID(_SessionID),
	m_ReceivedBytes(0),
	m_PendingIOCount(0),
	m_State(ESessionState::DISCONNECTED),
	m_Generation(0),
	m_SendOffset(0),
	m_QueuedSendBytes(0),
	m_IsSending(false),
	m_PacketDispatcher(_PacketDispatcher),
	m_LoginRequestPending(false)
{
	InitializeCriticalSection(&m_StateLock);
	InitializeCriticalSection(&m_SendLock);
	InitializeCriticalSection(&m_PendingIOLock);

	InitializeConditionVariable(&m_PendingIOCCondition);

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
	assert(m_ReceiveTask.IsDone());
	m_ReceiveTask.Reset();
	DeleteCriticalSection(&m_StateLock);
	DeleteCriticalSection(&m_SendLock);
	DeleteCriticalSection(&m_PendingIOLock);
}

ESessionState CSession::GetState()
{
	CCSLock Lock(&m_StateLock);
	return m_State;
}

uint64_t CSession::GetGeneration()
{
	CCSLock Lock(&m_StateLock);
	return m_Generation;
}

size_t CSession::GetQueuedSendBytes()
{
	CCSLock Lock(&m_SendLock);
	return m_QueuedSendBytes;
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
	m_LoginRequestPending = false;

	if (m_Socket != INVALID_SOCKET)
	{
		closesocket(m_Socket);
		m_Socket = INVALID_SOCKET;
	}
}

void CSession::Release()
{
	assert(m_PendingIOCount.load() == 0);
	assert(m_ReceiveTask.IsDone());
	m_ReceiveTask.Reset();
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

	if (m_State != ESessionState::DISCONNECTING || m_PendingIOCount.load() != 0 || m_LoginRequestPending || m_User)
	{
		return false;
	}

	// Pending completions also pin every in-progress resume call, including the
	// initial resume from ACCEPT. The final-suspended frame is now safe to destroy.
	assert(m_ReceiveTask.IsDone());
	m_ReceiveTask.Reset();

	CCSLock SendLock(&m_SendLock);

	memset(&m_AcceptOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_ReadOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_WriteOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	m_ReceivedBytes = 0;

	m_SendQueue.clear();
	m_SendOffset = 0;
	m_QueuedSendBytes = 0;
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
			m_PendingIOCount.fetch_sub(1);
			Release();
			m_State = ESessionState::DISCONNECTED;
			//Release();
			return false;
		}
	}

	return true;
}

bool CSession::StartReceiveLoop()
{
	{
		CCSLock Lock(&m_StateLock);
		if (m_State != ESessionState::CONNECTED)
		{
			return false;
		}
		m_ReceiveTask = ReceiveLoop(m_Generation);
	}

	// The ACCEPT completion remains pending until this initial resume returns.
	m_ReceiveTask.Resume();
	return GetState() == ESessionState::CONNECTED;
}

bool CSession::CompleteRead(DWORD _TransferredBytes, DWORD _Error)
{
	{
		CCSLock Lock(&m_StateLock);
		m_ReadResult = { _TransferredBytes, _Error };
	}

	// Failed/canceled completions must resume too, so the loop can terminate.
	// The caller releases this READ's pending count only after Resume returns.
	m_ReceiveTask.Resume();
	return GetState() == ESessionState::CONNECTED;
}

bool CSession::SReceiveAwaiter::await_suspend(std::coroutine_handle<>) const
{
	// Another IOCP worker may resume us before this function returns. Do not
	// access the awaiter/frame after PostRead publishes the operation.
	return Session.PostRead(Generation);
}

CSession::SReceiveResult CSession::SReceiveAwaiter::await_resume() const noexcept
{
	return Session.m_ReadResult;
}

CReceiveTask CSession::ReceiveLoop(uint64_t _Generation)
{
	while (true)
	{
		const SReceiveResult Result = co_await SReceiveAwaiter{ *this, _Generation };
		if (Result.Error != ERROR_SUCCESS || Result.Bytes == 0 || !OnRecv(Result.Bytes))
		{
			break;
		}
	}

	// IOCPManager performs user detachment and recycling after Resume returns.
	Disconnect();
}

bool CSession::PostRead(uint64_t _Generation)
{
	CCSLock Lock(&m_StateLock);
	m_ReadResult = {};
	if (m_State != ESessionState::CONNECTED || m_Generation != _Generation || m_ReceivedBytes >= sizeof(m_ReadBuffer))
	{
		return false;
	}

	memset(&m_ReadOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	WSABUF wsaBuf;
	wsaBuf.buf = m_ReadBuffer + m_ReceivedBytes;
	wsaBuf.len = static_cast<ULONG>(sizeof(m_ReadBuffer) - m_ReceivedBytes);

	DWORD flags = 0;

	m_PendingIOCount.fetch_add(1);
	int32_t result = WSARecv(m_Socket, &wsaBuf, 1, nullptr, &flags, &m_ReadOverlapped.Overlapped, nullptr);

	if (result == SOCKET_ERROR)
	{
		const DWORD Error = WSAGetLastError();
		if (Error != WSA_IO_PENDING)
		{
			m_ReadResult.Error = Error;
			m_PendingIOCount.fetch_sub(1);
			return false;
		}
	}

	// Immediate success still produces an IOCP completion. Only that completion
	// resumes the coroutine; returning false here would resume it twice.
	return true;
}

bool CSession::CompleteAccept()
{
	CCSLock Lock(&m_StateLock);

	if (m_State != ESessionState::ACCEPTING)
	{
		return false;
	}

	++m_Generation;
	m_State = ESessionState::CONNECTED;
	m_LoginRequestPending = false;

	return true;
}

bool CSession::Send(const void* _Data, size_t _Size, uint64_t _Generation)
{
	if (_Data == nullptr || _Size == 0)
	{
		return false;
	}

	bool StartWrite = false;

	{
		CCSLock StateLock(&m_StateLock);
		if (m_State != ESessionState::CONNECTED || m_Generation != _Generation)
		{
			return false;
		}

		CCSLock SendLock(&m_SendLock);
		if (_Size > MAX_QUEUED_SEND_BYTES || m_QueuedSendBytes > MAX_QUEUED_SEND_BYTES - _Size)
		{
			return false;
		}

		const char* Begin = static_cast<const char*>(_Data);
		m_SendQueue.emplace_back(Begin, Begin + _Size);
		m_QueuedSendBytes += _Size;
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
			m_QueuedSendBytes -= Packet.size();
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
	if (_TransferredBytes == 0 || _TransferredBytes > sizeof(m_ReadBuffer) - m_ReceivedBytes)
	{
		return false;
	}

	m_ReceivedBytes += _TransferredBytes;
	return ProcessPackets();
}

bool CSession::ProcessPackets()
{
	if (m_ReceivedBytes < sizeof(PacketHeader))
	{
		return true;
	}

	size_t BytesProcessed = 0;
	while (m_ReceivedBytes - BytesProcessed >= sizeof(PacketHeader))
	{
		PacketHeader Header{};
		memcpy(&Header, m_ReadBuffer + BytesProcessed, sizeof(Header));

		if (Header.packetSize < sizeof(PacketHeader) ||	Header.packetSize > sizeof(m_ReadBuffer))
		{
			return false;
		}

		if (m_ReceivedBytes - BytesProcessed < Header.packetSize)
		{
			break;
		}

		SPacketContext Context;
		Context.Source = EPacketSource::CLIENT;
		Context.Session = this;

		if (!m_PacketDispatcher.Dispatch(Context, m_ReadBuffer + BytesProcessed, Header))
		{
			return false;
		}

		BytesProcessed += Header.packetSize;
	}

	const size_t RemainingBytes = m_ReceivedBytes - BytesProcessed;
	memmove(m_ReadBuffer, m_ReadBuffer + BytesProcessed, RemainingBytes);
	m_ReceivedBytes = static_cast<DWORD>(RemainingBytes);

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

	return RemainingCount;
}

void CSession::WaitForPendingIO()
{
	CCSLock Lock(&m_PendingIOLock);
	while (m_PendingIOCount.load() != 0)
	{
		SleepConditionVariableCS(&m_PendingIOCCondition, &m_PendingIOLock, INFINITE);
	}
}

bool CSession::CompleteLogin(const shared_ptr<CUser>& _User, uint64_t _Generation)
{
	if(!_User)
	{
		return false;
	}

	CCSLock Lock(&m_StateLock);

	if (m_State != ESessionState::CONNECTED || m_Generation != _Generation || !m_LoginRequestPending || m_User)
	{
		return false;
	}

	m_User = _User;
	m_LoginRequestPending = false;

	return true;
}

bool CSession::IsAuthenticated(const shared_ptr<CUser>& _User, uint64_t _Generation)
{
	if (!_User)
	{
		return false;
	}

	CCSLock Lock(&m_StateLock);
	return m_State == ESessionState::CONNECTED && m_Generation == _Generation && !m_LoginRequestPending && m_User == _User;
}

shared_ptr<CUser> CSession::GetUser()
{
	CCSLock Lock(&m_StateLock);
	return m_User;
}

shared_ptr<CUser> CSession::DetachUser()
{
	CCSLock Lock(&m_StateLock);

	return move(m_User);
}

bool CSession::TryBeginLogin(uint64_t _Generation)
{
	CCSLock Lock(&m_StateLock);

	if (m_State != ESessionState::CONNECTED || m_Generation != _Generation || m_LoginRequestPending || m_User)
	{
		return false;
	}

	m_LoginRequestPending = true;
	return true;
}

bool CSession::IsLoginPending(uint64_t _Generation)
{
	CCSLock Lock(&m_StateLock);

	return m_State == ESessionState::CONNECTED && m_Generation == _Generation && m_LoginRequestPending && !m_User;
}

void CSession::CancelLogin(uint64_t _Generation)
{
	CCSLock Lock(&m_StateLock);

	if (m_Generation == _Generation && m_LoginRequestPending)
	{
		m_LoginRequestPending = false;
	}
}
