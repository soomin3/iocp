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

		bool keepConnected = success && ProcessCompletion(Session, Transferred, OverLappedData->ioType);

		if (!keepConnected)
		{
			Session->Disconnect();
		}

		Session->CompleteIO();
		TryRecycleSession(Session);
	}
}

void CIOCPManager::InitAcceptEx(SOCKET _ListenSoceket)
{
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	DWORD Bytes = 0;

	WSAIoctl(_ListenSoceket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &g_AcceptEx, sizeof(g_AcceptEx), &Bytes, nullptr, nullptr);
}

bool CIOCPManager::ProcessCompletion(CSession* _Session, DWORD _TransferredBytes, EIO_TYPE _IOType)
{
	switch (_IOType)
	{
	case EIO_TYPE::ACCEPT:
	{
		cout << "Client Connect\n";

		if (setsockopt(_Session->GetSocket(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&m_ServerSocket, sizeof(m_ServerSocket)) != 0)
		{
			cout << "setsockopt error: " << WSAGetLastError() << '\n';
			return false;
		}

		if (CreateIoCompletionPort((HANDLE)_Session->GetSocket(), m_IOCPHandle, 0, 0) == NULL)
		{
			cout << "CreateIoCompletionPort error: " << GetLastError() << '\n';
			return false;
		}

		return _Session->CompleteAccept() && _Session->PostRead();
	}
	case EIO_TYPE::READ:
	{
		//cout << "Read Complete\n";

		if (_TransferredBytes == 0) //클라이언트 종료
		{
			return false;
		}

		return _Session->OnRecv(_TransferredBytes) && _Session->PostRead();
	}
	case EIO_TYPE::WRITE:
	{
		cout << "Write Complete\n";

		return _Session->OnSend(_TransferredBytes);
	}
	default:
	{
		printf("Unknown IO Type\n");
		return false;
	}
	}

	return true;
}

void CIOCPManager::TryRecycleSession(CSession* _Session)
{
	if (_Session->TryReset() && m_IsRunning.load())
	{
		if (!_Session->PostAccept(m_ServerSocket))
		{
			cout << "PostAccept Failed\n";
		}
	}
}

void CIOCPManager::DisconnectSession(CSession* _Session)
{
	_Session->Disconnect();
	TryRecycleSession(_Session);
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

void CIOCPManager::PrintStatus()
{
	array<size_t, 4> SessionCounts{};
	int32_t TotalPendingIO = 0;

	for (const shared_ptr<CSession>& Session : m_SessionPool)
	{
		const size_t StateIndex =
			static_cast<size_t>(Session->GetState());

		if (StateIndex < SessionCounts.size())
		{
			++SessionCounts[StateIndex];
		}

		TotalPendingIO += Session->GetPendingIOCount();
	}

	cout << "running="
		<< (m_IsRunning.load() ? "true" : "false")
		<< ", workers=" << m_WorkerThreads.size()
		<< ", disconnected="
		<< SessionCounts[
			static_cast<size_t>(ESessionState::DISCONNECTED)]
		<< ", accepting="
		<< SessionCounts[
			static_cast<size_t>(ESessionState::ACCEPTING)]
		<< ", connected="
		<< SessionCounts[
			static_cast<size_t>(ESessionState::CONNECTED)]
		<< ", disconnecting="
		<< SessionCounts[
			static_cast<size_t>(ESessionState::DISCONNECTING)]
		<< ", pendingIO=" << TotalPendingIO
		<< '\n';
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
	if (::bind(m_ServerSocket, (sockaddr*)&ServerAddr, sizeof(sockaddr)) != 0)
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

	return;
}

void CIOCPManager::Stop()
{
	if (!m_IsRunning.exchange(false))
	{
		return;
	}

	if(m_ServerSocket != INVALID_SOCKET)
	{
		closesocket(m_ServerSocket);
	}

	for(shared_ptr<CSession>& session : m_SessionPool)
	{
		DisconnectSession(session.get());
	}

	for (shared_ptr<CSession>& session : m_SessionPool)
	{
		session->WaitForPendingIO();
	}

	StopWorkerThreads();
	m_ServerSocket = INVALID_SOCKET;
}

void CIOCPManager::Release()
{
	CloseHandle(m_IOCPHandle);
	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		m_SessionPool[i]->Release();
	}

	if(m_ServerSocket != INVALID_SOCKET)
	{
		closesocket(m_ServerSocket);
		m_ServerSocket = INVALID_SOCKET;
	}
	WSACleanup();
}
