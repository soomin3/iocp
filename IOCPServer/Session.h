#pragma once
#include "Myinclude.h"
#include <WinSock2.h>
#include <MSWSock.h>

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
};

class CSession
{
public:
	void Init();
	void Release();
	bool PostAccept(SOCKET _ListenSocket);
private:
	SOCKET m_Socket;

	SMyOverlapped m_AcceptOverlapped;
	SMyOverlapped m_WriteOverlapped;
	SMyOverlapped m_ReadOverlapped;

	char m_ReadBuffer[1024];
	char m_WriteBuffer[1024];
};