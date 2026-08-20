#include "pch.h"
#include "IOCPManager.h"

namespace
{
	constexpr ULONG_PTR IOCP_SHUTDOWN_KEY = 1;
}

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
			if (CompletionKey == IOCP_SHUTDOWN_KEY)
			{
				break;
			}

			continue;
		}

		SMyOverlapped* OverLappedData = reinterpret_cast<SMyOverlapped*>(OverLapped);
		CSession* Session = OverLappedData->owner;

		Session->CompleteIO();

		if (!success)
		{
			DisconnectSession(Session);
			continue;
		}

		if (OverLappedData->ioType == EIO_TYPE::ACCEPT)
		{
			cout << "Client Connect\n";

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

			if (Transferred == 0) //클라이언트 종료
			{
				DisconnectSession(Session);
				continue;
			}

			if (Session->OnRecv(Transferred) == false)
			{
				DisconnectSession(Session);
				continue;
			}

			if (Session->PostRead() == false)
			{
				DisconnectSession(Session);
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

void CIOCPManager::StopWorkerThreads()
{
	m_IsRunning.store(false);

	for(size_t i = 0; i < m_WorkerThreads.size(); ++i)
	{
		PostQueuedCompletionStatus(m_IOCPHandle, 0, IOCP_SHUTDOWN_KEY, nullptr);
	}

	for(auto& thread : m_WorkerThreads)
	{
		if (thread.joinable())
		{
			thread.join();
		}
	}

	m_WorkerThreads.clear();
}

CIOCPManager::CIOCPManager(CPacketDispatcher& _PacketDispatcher) : m_PacketDispatcher(_PacketDispatcher), m_IsRunning(false)
{
}

CIOCPManager::~CIOCPManager()
{
}

bool CIOCPManager::Init()
{
	WSADATA WsaData;
	int32_t WsaResult = WSAStartup(MAKEWORD(2, 2), &WsaData);
	if (WsaResult != 0)
	{
		printf("WSAStartup error: %d\n", WsaResult);
		return false;
	}

	m_ServerSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_ServerSocket == INVALID_SOCKET)
	{
		printf("socket error: %d\n", WSAGetLastError());
		return false;
	}

	InitAcceptEx(m_ServerSocket);

	sockaddr_in ServerAddr;
	ServerAddr.sin_family = AF_INET;
	ServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	ServerAddr.sin_port = htons(8694);
	if (bind(m_ServerSocket, (sockaddr*)&ServerAddr, sizeof(sockaddr)) != 0)
	{
		printf("bind error: %d\n", WSAGetLastError());
		return false;
	}

	if (listen(m_ServerSocket, 5) == SOCKET_ERROR)
	{
		printf("listen error: %d\n", WSAGetLastError());
		return false;
	}

	m_IOCPHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (!m_IOCPHandle)
	{
		printf("CreateIoCompletionPort error: %d\n", GetLastError());
		return false;
	}

	CreateIoCompletionPort((HANDLE)m_ServerSocket, m_IOCPHandle, 0, 0);

	m_SessionPool.reserve(USER_SIZE);
	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		m_SessionPool.push_back(make_shared<CSession>(m_PacketDispatcher));
	}

	return true;
}

void CIOCPManager::Run()
{
	m_IsRunning.store(true);

	uint32_t ThreadCount = thread::hardware_concurrency() * 2;
	ThreadCount = ThreadCount == 0 ? 4 : ThreadCount;

	for (uint32_t i = 0; i < ThreadCount; ++i)
	{
		m_WorkerThreads.emplace_back([this]()
		{
			WorkerThread(m_IOCPHandle);
		});
	}

	DWORD ReceiveSize = 0;

	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
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
