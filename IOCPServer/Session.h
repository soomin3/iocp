#pragma once
#include <WinSock2.h>
#include <MSWSock.h>

using namespace std;
class CSession;
class CPacketDispatcher;

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
	explicit CSession(const CPacketDispatcher& _PacketDispatcher);
	~CSession();

	SOCKET GetSocket() const { return m_Socket; }

	void Init();
	void Disconnect();
	void Release();
	bool TryReset();
	bool PostAccept(SOCKET _ListenSocket);
	bool PostRead();
	bool CompleteAccept();

	bool Send(const void* _Data, size_t _Size);
	bool PostWrite();
	bool OnSend(DWORD _TransferredBytes);

	bool OnRecv(DWORD _TransferredBytes);

	int32_t CompleteIO();
	void WaitForPendingIO();

	void PrintReadBuffer()
	{
		cout << "Read Buffer: " << m_ReadBuffer << endl;
	}

private:
	SOCKET m_Socket;

	SMyOverlapped m_AcceptOverlapped;
	SMyOverlapped m_WriteOverlapped;
	SMyOverlapped m_ReadOverlapped;

	char m_AcceptBuffer[64];
	char m_ReadBuffer[1024];

	DWORD m_ReceivedBytes;

	atomic_int32_t m_PendingIOCount;

	CRITICAL_SECTION m_StateLock;
	ESessionState m_State;

	CRITICAL_SECTION m_SendLock;
	deque<vector<char>> m_SendQueue;
	size_t m_SendOffset;
	bool m_IsSending;
	CRITICAL_SECTION m_PendingIOLock;
	CONDITION_VARIABLE m_PendingIOCCondition;

	const CPacketDispatcher& m_PacketDispatcher;
};