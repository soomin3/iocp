#pragma once
#include <WinSock2.h>
#include "Myinclude.h"
#include <MSWSock.h>

using namespace std;

enum class EIO_TYPE : uint8_t
{
	ACCEPT,
	READ,
	WRITE
};

struct SMyOverlapped
{
	WSAOVERLAPPED Overlapped;
	EIO_TYPE ioType;
	SOCKET ClientSocket;
	int32_t SessionIndex;
};

class CSession
{
public:
	CSession();
	~CSession() = default;
	void Init();
	void Release();
	bool PostAccept(SOCKET _ListenSocket, int32_t _SessionIndex);
	bool PostRead(int32_t _SessionIndex);

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
};