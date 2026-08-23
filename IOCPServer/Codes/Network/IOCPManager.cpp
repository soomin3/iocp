#include "pch.h"
#include "IOCPManager.h"

#include <algorithm>
#include <system_error>

#include "Packet.h"
#include "PacketDispatcher.h"
#include "UserManager.h"

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

			if (CompletionKey != 0)
			{
				unique_ptr<PacketBuffer> Packet(reinterpret_cast<PacketBuffer*>(CompletionKey));

				if (Packet->size() >= sizeof(PacketHeader))
				{
					PacketHeader Header;
					memcpy(&Header, Packet->data(), sizeof(Header));

					if (Header.packetSize == Packet->size())
					{
						SPacketContext Context;
						Context.Source = EPacketSource::DATABASE;

						if (Header.packetID == PacketID::DS_LOGIN_RES &&
							Packet->size() == sizeof(DS_LOGIN_RES))
						{
							DS_LOGIN_RES Response;
							memcpy(&Response, Packet->data(), sizeof(Response));

							if (Response.sessionID < m_SessionPool.size())
							{
								Context.Session = m_SessionPool[Response.sessionID].get();
							}
						}

						m_PacketDispatcher.Dispatch(Context, Packet->data(), Header);
					}
				}
			}

			continue;
		}

		SMyOverlapped* OverLappedData = reinterpret_cast<SMyOverlapped*>(OverLapped);
		CSession* Session = OverLappedData->owner;

		bool keepConnected = success && ProcessCompletion(Session, Transferred, OverLappedData->ioType);

		if (!keepConnected)
		{
			DisconnectSession(Session);
		}

		Session->CompleteIO();
		TryRecycleSession(Session);
	}
}

bool CIOCPManager::InitAcceptEx(SOCKET _ListenSocket)
{
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	DWORD Bytes = 0;

	const int32_t Result = WSAIoctl(
		_ListenSocket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidAcceptEx,
		sizeof(GuidAcceptEx),
		&g_AcceptEx,
		sizeof(g_AcceptEx),
		&Bytes,
		nullptr,
		nullptr);

	if (Result == SOCKET_ERROR || g_AcceptEx == nullptr)
	{
		cout << "AcceptEx initialization failed: " << WSAGetLastError() << '\n';
		return false;
	}

	return true;
}

bool CIOCPManager::ProcessCompletion(CSession* _Session, DWORD _TransferredBytes, EIO_TYPE _IOType)
{
	switch (_IOType)
	{
	case EIO_TYPE::ACCEPT:
	{
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
		return _Session->OnSend(_TransferredBytes);
	}
	default:
	{
		printf("Unknown IO Type\n");
		return false;
	}
	}
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
	if (_Session == nullptr)
	{
		return;
	}

	_Session->Disconnect();

	shared_ptr<CUser> User = _Session->DetachUser();

	if (User)
	{
		m_UserManager.RemoveUser(User->GetAccountID(), User);
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

void CIOCPManager::PrintStatus()
{
	array<size_t, 4> SessionCounts{};
	int32_t TotalPendingIO = 0;

	for (const shared_ptr<CSession>& Session : m_SessionPool)
	{
		const size_t StateIndex = static_cast<size_t>(Session->GetState());

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

CIOCPManager::CIOCPManager(CPacketDispatcher& _PacketDispatcher,CUserManager& _UserManager)
	: m_ServerSocket(INVALID_SOCKET),
	m_IOCPHandle(nullptr),
	m_WsaStarted(false),
	m_PacketDispatcher(_PacketDispatcher),
	m_IsRunning(false),
	m_UserManager(_UserManager)
{
}

CIOCPManager::~CIOCPManager()
{
	Stop();
	Release();
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
	m_WsaStarted = true;

	m_ServerSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_ServerSocket == INVALID_SOCKET)
	{
		printf("socket error: %d\n", WSAGetLastError());
		Release();
		return false;
	}

	if (!InitAcceptEx(m_ServerSocket))
	{
		Release();
		return false;
	}

	BOOL ExclusiveAddressUse = TRUE;
	if (setsockopt(m_ServerSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&ExclusiveAddressUse), sizeof(ExclusiveAddressUse)) == SOCKET_ERROR)
	{
		printf("SO_EXCLUSIVEADDRUSE error: %d\n", WSAGetLastError());
		Release();
		return false;
	}

	sockaddr_in ServerAddr{};
	ServerAddr.sin_family = AF_INET;
	ServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	ServerAddr.sin_port = htons(8694);
	if (::bind(m_ServerSocket, (sockaddr*)&ServerAddr, sizeof(sockaddr)) != 0)
	{
		printf("bind error: %d\n", WSAGetLastError());
		Release();
		return false;
	}

	if (listen(m_ServerSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		printf("listen error: %d\n", WSAGetLastError());
		Release();
		return false;
	}

	m_IOCPHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (!m_IOCPHandle)
	{
		printf("CreateIoCompletionPort error: %d\n", GetLastError());
		Release();
		return false;
	}

	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(m_ServerSocket), m_IOCPHandle, 0, 0) == nullptr)
	{
		printf("Listen socket IOCP association error: %d\n", GetLastError());
		Release();
		return false;
	}

	m_SessionPool.reserve(USER_SIZE);
	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		m_SessionPool.push_back(make_shared<CSession>(m_PacketDispatcher, static_cast<uint32_t>(i)));
	}

	return true;
}

bool CIOCPManager::Run()
{
	if (m_IOCPHandle == nullptr || m_ServerSocket == INVALID_SOCKET || m_IsRunning.exchange(true))
	{
		return false;
	}

	uint32_t ThreadCount = thread::hardware_concurrency() * 2;
	ThreadCount = ThreadCount == 0 ? 4 : ThreadCount;
	ThreadCount = min<uint32_t>(ThreadCount, 32);

	try
	{
		for (uint32_t i = 0; i < ThreadCount; ++i)
		{
			m_WorkerThreads.emplace_back([this]()
			{
				WorkerThread(m_IOCPHandle);
			});
		}
	}
	catch (const system_error& Error)
	{
		cout << "IOCP worker start failed: " << Error.what() << '\n';
		Stop();
		return false;
	}

	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		if (!m_SessionPool[i]->PostAccept(m_ServerSocket))
		{
			printf("PostAccept error: %d\n", WSAGetLastError());
			Stop();
			return false;
		}
	}

	//cout << ThreadCount << endl;
	cout << "Server is running on port 8694..." << endl;

	return true;
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
		m_ServerSocket = INVALID_SOCKET;
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
}

bool CIOCPManager::SendPacket(PacketBuffer _Packet)
{
	if (_Packet.size() < sizeof(PacketHeader) || m_IOCPHandle == nullptr)
	{
		return false;
	}

	PacketHeader Header;
	memcpy(&Header, _Packet.data(), sizeof(Header));
	if (Header.packetSize != _Packet.size())
	{
		return false;
	}

	unique_ptr<PacketBuffer> Packet = make_unique<PacketBuffer>(move(_Packet));
	PacketBuffer* PostedPacket = Packet.release();

	if (!PostQueuedCompletionStatus(m_IOCPHandle, 0, reinterpret_cast<ULONG_PTR>(PostedPacket), nullptr))
	{
		delete PostedPacket;
		return false;
	}

	return true;
}

void CIOCPManager::Release()
{
	if (m_IsRunning.load())
	{
		Stop();
	}

	for (shared_ptr<CSession>& Session : m_SessionPool)
	{
		Session->Release();
	}
	m_SessionPool.clear();

	if(m_ServerSocket != INVALID_SOCKET)
	{
		closesocket(m_ServerSocket);
		m_ServerSocket = INVALID_SOCKET;
	}

	if (m_IOCPHandle != nullptr)
	{
		CloseHandle(m_IOCPHandle);
		m_IOCPHandle = nullptr;
	}

	if (m_WsaStarted)
	{
		WSACleanup();
		m_WsaStarted = false;
	}

	g_AcceptEx = nullptr;
}
