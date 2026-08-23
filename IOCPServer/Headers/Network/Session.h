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

using namespace std;
class CSession;
class CPacketDispatcher;
class CUser;

enum class EIO_TYPE : uint8_t
{
	ACCEPT,
	READ,
	WRITE
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
	bool PostAccept(SOCKET _ListenSocket);
	bool PostRead();
	bool CompleteAccept();

	bool Send(const void* _Data, size_t _Size, uint64_t _Generation);
	bool PostWrite();
	bool OnSend(DWORD _TransferredBytes);

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
	bool ProcessPackets();

	static constexpr size_t READ_BUFFER_SIZE = 64 * 1024;

	SOCKET m_Socket;
	const uint32_t m_SessionID;

	SMyOverlapped m_AcceptOverlapped;
	SMyOverlapped m_WriteOverlapped;
	SMyOverlapped m_ReadOverlapped;

	char m_AcceptBuffer[64];
	char m_ReadBuffer[READ_BUFFER_SIZE];

	DWORD m_ReceivedBytes;

	atomic_int32_t m_PendingIOCount;

	CRITICAL_SECTION m_StateLock;
	ESessionState m_State;
	uint64_t m_Generation;

	CRITICAL_SECTION m_SendLock;
	deque<vector<char>> m_SendQueue;
	size_t m_SendOffset;
	size_t m_QueuedSendBytes;
	bool m_IsSending;
	CRITICAL_SECTION m_PendingIOLock;
	CONDITION_VARIABLE m_PendingIOCCondition;

	const CPacketDispatcher& m_PacketDispatcher;
	shared_ptr<CUser> m_User;
	bool m_LoginRequestPending;

	static constexpr size_t MAX_QUEUED_SEND_BYTES = 1024 * 1024;
};
