#include "Session.h"

extern LPFN_ACCEPTEX g_AcceptEx;

CSession::CSession()
{
	m_Socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

	memset(&m_AcceptOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_ReadOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));
	memset(&m_WriteOverlapped.Overlapped, 0, sizeof(WSAOVERLAPPED));

	m_AcceptOverlapped.ioType = EIO_TYPE::ACCEPT;
	m_AcceptOverlapped.ClientSocket = m_Socket;

	m_ReadOverlapped.ioType = EIO_TYPE::READ;
	m_ReadOverlapped.ClientSocket = m_Socket;

	m_WriteOverlapped.ioType = EIO_TYPE::WRITE;
	m_WriteOverlapped.ClientSocket = m_Socket;
}

void CSession::Init()
{
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
}

bool CSession::PostAccept(SOCKET _ListenSocket, int32_t _SessionIndex)
{
	m_AcceptOverlapped.SessionIndex = _SessionIndex;

	const int32_t AddrSize = sizeof(sockaddr_in) + 16;
	DWORD ReceiveSize = 0;

	g_AcceptEx(_ListenSocket, m_Socket, m_AcceptBuffer, 0, AddrSize, AddrSize, &ReceiveSize, (LPOVERLAPPED)&m_AcceptOverlapped);

	return false;
}

bool CSession::PostRead(int32_t _SessionIndex)
{
	memset(m_ReadBuffer, 0, sizeof(m_ReadBuffer));
	m_ReadOverlapped.SessionIndex = _SessionIndex;

	WSABUF wsaBuf;
	wsaBuf.buf = m_ReadBuffer;
	wsaBuf.len = sizeof(m_ReadBuffer);

	DWORD bytesReceived = 0;
	DWORD flags = 0;

	int32_t result = WSARecv(m_Socket, &wsaBuf, 1, &bytesReceived, &flags, (LPWSAOVERLAPPED)&m_ReadOverlapped, nullptr);

	if (result == SOCKET_ERROR)
	{
		if (WSAGetLastError() != WSA_IO_PENDING) 
		{
			return false;
		}
	}
	return true;
}
