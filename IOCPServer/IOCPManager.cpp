#include "IOCPManager.h"
//#include <WinSock2.h>
#include <MSWSock.h>
#include <thread>

LPFN_ACCEPTEX g_AcceptEx = nullptr;

void CIOCPManager::WorkerThread(HANDLE _iocpHandle)
{
	while (true)
	{
		DWORD Transferred = 0;
		ULONG_PTR CompletionKey = 0;
		LPOVERLAPPED OverLapped = nullptr;

		bool success = GetQueuedCompletionStatus(_iocpHandle, &Transferred, &CompletionKey, &OverLapped, INFINITE);

		if (!success || !OverLapped)
		{
			//printf("GetQueuedCompletionStatus error: %d\n", GetLastError());
			continue;
		}

		SMyOverlapped* OverLappedData = reinterpret_cast<SMyOverlapped*>(OverLapped);

		if (OverLappedData->ioType == EIO_TYPE::ACCEPT)
		{
			cout << "Client Connect\n";

			setsockopt(OverLappedData->ClientSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&m_ServerSocket, sizeof(m_ServerSocket));
			CreateIoCompletionPort((HANDLE)OverLappedData->ClientSocket, _iocpHandle, 0, 0);
		}
	}
}

void CIOCPManager::InitAcceptEx(SOCKET _ListenSoceket)
{
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	DWORD Bytes = 0;

	WSAIoctl(_ListenSoceket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &g_AcceptEx, sizeof(g_AcceptEx), &Bytes, nullptr, nullptr);
}

void CIOCPManager::Init()
{
	WSADATA WsaData;
	int32_t WsaResult = WSAStartup(MAKEWORD(2, 2), &WsaData);
	if (WsaResult != 0)
	{
		printf("WSAStartup error: %d\n", WsaResult);
		return;
	}

	m_ServerSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_ServerSocket == INVALID_SOCKET)
	{
		printf("socket error: %d\n", WSAGetLastError());
		return;
	}

	InitAcceptEx(m_ServerSocket);

	sockaddr_in ServerAddr;
	ServerAddr.sin_family = AF_INET;
	ServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	ServerAddr.sin_port = htons(8694);
	if (bind(m_ServerSocket, (sockaddr*)&ServerAddr, sizeof(sockaddr)) != 0)
	{
		printf("bind error: %d\n", WSAGetLastError());
		return;
	}

	if (listen(m_ServerSocket, 5) == SOCKET_ERROR)
	{
		printf("listen error: %d\n", WSAGetLastError());
		return;
	}

	m_IOCPHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (!m_IOCPHandle)
	{
		printf("CreateIoCompletionPort error: %d\n", GetLastError());
		return;
	}

	CreateIoCompletionPort((HANDLE)m_ServerSocket, m_IOCPHandle, 0, 0);

	m_SessionPool.reserve(USER_SIZE);
	//m_AcceptSockPool.reserve(ACCEPT_SOCK_SIZE);
	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		unique_ptr<CSession> Session = make_unique<CSession>();
		Session->Init();
		m_SessionPool.push_back(move(Session));
		//m_AcceptSockPool.push_back(WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED));
	}

	//int32_t MaxOverlappedCount = ACCEPT_SOCK_SIZE * 3;
	//m_OverlappedPool.reserve(MaxOverlappedCount);
	//for (int32_t i = 0; i < MaxOverlappedCount; ++i)
	//{
	//	m_OverlappedPool.push_back(make_unique<SMyOverlapped>());
	//}
}

void CIOCPManager::Run()
{
	vector<thread> workerThreads;
	uint32_t ThreadCount = thread::hardware_concurrency() * 2;

	for (uint32_t i = 0; i < ThreadCount; ++i)
	{
		workerThreads.emplace_back([this]()
		{
			WorkerThread(m_IOCPHandle);
		});
	}

	//sockaddr_in ClientAddr = {};
	//int32_t ClientAddrSize = sizeof(ClientAddr);

	//const int32_t DataSize = 1024;
	//const int32_t AddrSize = sizeof(sockaddr_in) + 16;

	//SOCKET ClientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	DWORD ReceiveSize = 0;
	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		/*SMyOverlapped* AcceptOverlapped = m_OverlappedPool[i].get();
		AcceptOverlapped->ioType = EIO_TYPE::ACCEPT;
		AcceptOverlapped->ClientSocket = m_AcceptSockPool[i];

		BOOL AcceptResult = g_AcceptEx(m_ServerSocket, m_AcceptSockPool[i], AcceptOverlapped->RecvBuffer, 0, AddrSize, AddrSize, &ReceiveSize, (LPOVERLAPPED)AcceptOverlapped);*/
		bool AcceptResult = m_SessionPool[i]->PostAccept(m_ServerSocket);
		if (AcceptResult == false && WSAGetLastError() != WSA_IO_PENDING)
		{
			printf("accept error: %d\n", WSAGetLastError());
			return;
		}
	}

	cout << ThreadCount << endl;

	while (true)
	{
	}

	Release();

	return;
}

void CIOCPManager::Release()
{
	CloseHandle(m_IOCPHandle);
	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		m_SessionPool[i]->Release();
	}
	closesocket(m_ServerSocket);
	WSACleanup();
}
