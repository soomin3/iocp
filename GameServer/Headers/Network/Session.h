#pragma once
#include <WinSock2.h>
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <vector>
#include <memory>

#include "Myinclude.h"
#include "IOTask.h"

using namespace std;
class CSession;
class CPacketDispatcher;
class CUser;

enum class EIO_TYPE : uint8_t
{
	ACCEPT,
	READ,
	WRITE,
	SEND_WAKE
};

enum class ESessionState : uint8_t
{
	DISCONNECTED,
	ACCEPTING,
	CONNECTED,
	DISCONNECTING,
};

struct SMyOverlapped
{
	WSAOVERLAPPED Overlapped;
	EIO_TYPE ioType;
	CSession* owner;
};

class CSession
{
public:
	CSession(const CPacketDispatcher& _PacketDispatcher, uint32_t _SessionID);
	~CSession();

	SOCKET GetSocket() const { return m_Socket; }
	uint32_t GetSessionID() const { return m_SessionID; }
	int32_t GetPendingIOCount() const { return m_PendingIOCount.load(); }
	size_t GetQueuedSendBytes();

	ESessionState GetState();
	uint64_t GetGeneration();

	void Init();
	void Disconnect();
	void Release();
	bool TryReset();
	bool StartAccept(SOCKET _ListenSocket, HANDLE _IOCPHandle);
	bool CompleteAccept(DWORD _Error);
	bool StartReceiveLoop();
	bool CompleteRead(DWORD _TransferredBytes, DWORD _Error);

	bool Send(const void* _Data, size_t _Size, uint64_t _Generation);
	bool StartSendLoop(HANDLE _IOCPHandle);
	bool ResumeSendLoop();
	bool CompleteWrite(DWORD _TransferredBytes, DWORD _Error);

	bool OnRecv(DWORD _TransferredBytes);

	int32_t CompleteIO();
	void WaitForPendingIO();

	bool CompleteLogin(const shared_ptr<CUser>& _User, uint64_t _Generation);
	bool IsAuthenticated(const shared_ptr<CUser>& _User, uint64_t _Generation);
	shared_ptr<CUser> GetUser();
	shared_ptr<CUser> DetachUser();

	bool TryBeginLogin(uint64_t _Generation);
	bool IsLoginPending(uint64_t _Generation);
	void CancelLogin(uint64_t _Generation);

	void PrintReadBuffer()
	{
		cout << "Read Buffer: " << m_ReadBuffer << endl;
	}

private:
	struct SIOResult
	{
		DWORD Bytes = 0;
		DWORD Error = ERROR_OPERATION_ABORTED;
	};

	struct SAcceptAwaiter
	{
		CSession& Session;
		SOCKET ListenSocket;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<>) const;
		DWORD await_resume() const noexcept;
	};

	struct SReceiveAwaiter
	{
		CSession& Session;
		uint64_t Generation;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<>) const;
		SIOResult await_resume() const noexcept;
	};

	struct SSendQueueAwaiter
	{
		CSession& Session;
		uint64_t Generation;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<>) const;
		bool await_resume() const;
	};

	struct SSendAwaiter
	{
		CSession& Session;
		uint64_t Generation;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<>) const;
		SIOResult await_resume() const noexcept;
	};

	CIOTask AcceptConnection(SOCKET _ListenSocket, HANDLE _IOCPHandle);
	bool PostAccept(SOCKET _ListenSocket);
	bool InitializeAcceptedSocket(SOCKET _ListenSocket, HANDLE _IOCPHandle);
	CIOTask ReceiveLoop(uint64_t _Generation);
	bool PostRead(uint64_t _Generation);
	CIOTask SendLoop(uint64_t _Generation);
	bool WaitForSend(uint64_t _Generation);
	bool PostWrite(uint64_t _Generation);
	bool OnSend(DWORD _TransferredBytes);
	bool ProcessPackets();

	static constexpr size_t READ_BUFFER_SIZE = 64 * 1024;

	SOCKET m_Socket;
	const uint32_t m_SessionID;

	SMyOverlapped m_AcceptOverlapped;
	SMyOverlapped m_WriteOverlapped;
	SMyOverlapped m_ReadOverlapped;
	SMyOverlapped m_SendWakeOverlapped;

	char m_AcceptBuffer[64];
	DWORD m_AcceptError = ERROR_OPERATION_ABORTED;
	CIOTask m_AcceptTask;
	char m_ReadBuffer[READ_BUFFER_SIZE];

	DWORD m_ReceivedBytes;
	SIOResult m_ReadResult;
	CIOTask m_ReceiveTask;

	atomic_int32_t m_PendingIOCount;

	CRITICAL_SECTION m_StateLock;
	ESessionState m_State;
	uint64_t m_Generation;

	CRITICAL_SECTION m_SendLock;
	deque<vector<char>> m_SendQueue;
	size_t m_SendOffset;
	size_t m_QueuedSendBytes;
	bool m_SendWaiting;
	HANDLE m_IOCPHandle;
	SIOResult m_WriteResult;
	CIOTask m_SendTask;
	CRITICAL_SECTION m_PendingIOLock;
	CONDITION_VARIABLE m_PendingIOCCondition;

	const CPacketDispatcher& m_PacketDispatcher;
	shared_ptr<CUser> m_User;
	bool m_LoginRequestPending;

	static constexpr size_t MAX_QUEUED_SEND_BYTES = 1024 * 1024;
};
