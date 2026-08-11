#include "pch.h"
#include "IOCPManager.h"
#include "PacketManager.h"

LPFN_ACCEPTEX g_AcceptEx = nullptr;

void CIOCPManager::WorkerThread(HANDLE _iocpHandle)
{
	while (true)
	{
		DWORD Transferred = 0;
		ULONG_PTR CompletionKey = 0;
		LPOVERLAPPED OverLapped = nullptr;

		bool success = GetQueuedCompletionStatus(_iocpHandle, &Transferred, &CompletionKey, &OverLapped, INFINITE);

		if (OverLapped == nullptr)
		{
			continue;
		}

		SMyOverlapped* OverLappedData = reinterpret_cast<SMyOverlapped*>(OverLapped);
		CSession* Session = OverLappedData->owner;

		Session->CompleteIO();

		if (!success)
		{
			DisconnectSession(Session);
			//Session->Release();
			//Session->PostAccept(m_ServerSocket);
			continue;
		}

		if (OverLappedData->ioType == EIO_TYPE::ACCEPT)
		{
			cout << "Client Connect\n";
			//int32_t SessionIndex = OverLappedData->SessionIndex;
			//CSession* Session = m_SessionPool[SessionIndex].get();
			//shared_ptr<CUser> user = m_UserManager.PopUser();
			if (setsockopt(Session->GetSocket(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&m_ServerSocket, sizeof(m_ServerSocket)) != 0)
			{
				cout << "setsockopt error: " << WSAGetLastError() << '\n';
				DisconnectSession(Session);
				continue;
			}
			if (CreateIoCompletionPort((HANDLE)Session->GetSocket(), _iocpHandle, 0, 0) == NULL)
			{
				cout << "CreateIoCompletionPort error: " << GetLastError() << '\n';
				DisconnectSession(Session);
				continue;
			}

			if (!Session->CompleteAccept() || !Session->PostRead())
			{
				DisconnectSession(Session);
			}
		}
		else if (OverLappedData->ioType == EIO_TYPE::READ)
		{
			//cout << "Read Complete\n";
			//int32_t SessionIndex = OverLappedData->SessionIndex;

			if (Transferred == 0) //클라이언트 종료
			{
				DisconnectSession(Session);
				//Session->Release();
				//cout << "Client Disconnected\n";
				//Session->PostAccept(m_ServerSocket);
				continue;
			}

			//m_SessionPool[SessionIndex]->PrintReadBuffer();
			if (Session->OnRecv(Transferred, m_PacketManager.get()) == false)
			{
				DisconnectSession(Session);
				//Session->Release();
				//Session->PostAccept(m_ServerSocket);
				continue;
			}

			if (Session->PostRead() == false)
			{
				DisconnectSession(Session);
				//Session->Release();
				//Session->PostAccept(m_ServerSocket);
				continue;
			}
		}
		else if (OverLappedData->ioType == EIO_TYPE::WRITE)
		{
			cout << "Write Complete\n";

			if (!Session->OnSend(Transferred))
			{
				DisconnectSession(Session);
			}
		}
		else
		{
			printf("Unknown IO Type\n");
		}
	}
}

void CIOCPManager::InitAcceptEx(SOCKET _ListenSoceket)
{
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	DWORD Bytes = 0;

	WSAIoctl(_ListenSoceket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &g_AcceptEx, sizeof(g_AcceptEx), &Bytes, nullptr, nullptr);
}

void CIOCPManager::DisconnectSession(CSession* _Session)
{
	_Session->Disconnect();

	if (_Session->TryReset())
	{
		if (!_Session->PostAccept(m_ServerSocket))
		{
			cout << "PostAccept Failed\n";
		}
	}
}

CIOCPManager::CIOCPManager()
{
	m_PacketManager = make_unique<CPacketManager>();
}

CIOCPManager::~CIOCPManager()
{
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

	//m_UserManager.Init();

	m_SessionPool.reserve(USER_SIZE);
	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		m_SessionPool.push_back(make_shared<CSession>());
	}
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

	//cout << ThreadCount << endl;
	cout << "Server is running on port 8694..." << endl;

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
