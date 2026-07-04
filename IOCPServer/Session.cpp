#include "Session.h"

extern LPFN_ACCEPTEX g_AcceptEx;

void CSession::Init()
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

bool CSession::PostAccept(SOCKET _ListenSocket)
{
	const int32_t AddrSize = sizeof(sockaddr_in) + 16;
	DWORD ReceiveSize = 0;

	g_AcceptEx(_ListenSocket, m_Socket, m_ReadBuffer, 0, AddrSize, AddrSize, &ReceiveSize, (LPOVERLAPPED)&m_AcceptOverlapped);

	return false;
}
