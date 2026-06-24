#include <stdint.h>
#include <stdio.h>
#include <WinSock2.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <iostream>

using namespace std;

enum class IO_TYPE : uint8_t
{
	ACCEPT,
	READ,
	WRITE
};

struct MyOverlapped
{
	WSAOVERLAPPED Overlapped;
	IO_TYPE ioType;
};

void WorkerThread(HANDLE _iocpHandle)
{
	while (true)
	{
		DWORD Transferred = 0;
		ULONG_PTR CompletionKey = 0;
		LPOVERLAPPED OverLapped = nullptr;

		bool success = GetQueuedCompletionStatus(_iocpHandle, &Transferred, &CompletionKey, &OverLapped, INFINITE);

		if(!success)
		{
			printf("GetQueuedCompletionStatus error: %d\n", GetLastError());
			continue;
		}
	}
}

LPFN_ACCEPTEX g_AcceptEx = nullptr;

void InitAcceptEx(SOCKET _ListenSoceket)
{
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	DWORD Bytes = 0;

	WSAIoctl(_ListenSoceket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &g_AcceptEx, sizeof(g_AcceptEx), &Bytes, nullptr, nullptr);
}

int main(int argc, char** argv)
{
	WSADATA WsaData;
	int32_t WsaResult = WSAStartup(MAKEWORD(2, 2), &WsaData);
	if (WsaResult != 0)
	{
		printf("WSAStartup error: %d\n", WsaResult);
		return 1;
	}

	//SOCKET ServerSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKET ServerSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (ServerSocket == INVALID_SOCKET)
	{
		printf("socket error: %d\n", WSAGetLastError());
		return 1;
	}

	InitAcceptEx(ServerSocket);

	sockaddr_in ServerAddr;
	ServerAddr.sin_family = AF_INET;
	ServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	ServerAddr.sin_port = htons(8694);
	if (bind(ServerSocket, (sockaddr*)&ServerAddr, sizeof(sockaddr)) != 0)
	{
		printf("bind error: %d\n", WSAGetLastError());
		return 1;
	}

	if (listen(ServerSocket, 5) == SOCKET_ERROR)
	{
		printf("listen error: %d\n", WSAGetLastError());
		return 1;
	}

	sockaddr_in ClientAddr = {};
	int32_t ClientAddrSize = sizeof(ClientAddr);
	//SOCKET ClientSocket = accept(ServerSocket, (sockaddr*)&ClientAddr, &ClientAddrSize);
	//if (ClientSocket == INVALID_SOCKET)
	//{
	//	printf("accept error: %d\n", WSAGetLastError());
	//	return 1;
	//}

	const int32_t DataSize = 1024;
	const int32_t AddrSize = sizeof(sockaddr_in) + 16;
	char RecvBuffer[DataSize + (AddrSize * 2)] = { 0, };

	MyOverlapped AcceptOverlapped = {};
	AcceptOverlapped.ioType = IO_TYPE::ACCEPT;

	SOCKET ClientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	DWORD ReceiveSize = 0;
	BOOL AcceptResult = g_AcceptEx(ServerSocket, ClientSocket, RecvBuffer, DataSize, AddrSize, AddrSize, &ReceiveSize, (LPOVERLAPPED)&AcceptOverlapped);

	if (AcceptResult == FALSE && WSAGetLastError() != WSA_IO_PENDING)
	{
		printf("accept error: %d\n", WSAGetLastError());
		return 1;
	}

	HANDLE iocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (!iocpHandle)
	{
		printf("CreateIoCompletionPort error: %d\n", GetLastError());
		return 1;
	}

	uint32_t ThreadCount = thread::hardware_concurrency() * 2;
	vector<thread> workerThreads;

	for (int32_t i = 0; i < ThreadCount; ++i)
	{
		workerThreads.emplace_back(WorkerThread, iocpHandle);
	}

	cout << ThreadCount << endl;

	for (auto& th : workerThreads)
	{
		if (th.joinable())
		{
			th.join();
		}
	}

	char Message[] = "Hello, World!";
	send(ClientSocket, Message, sizeof(Message), 0);

	CloseHandle(iocpHandle);
	closesocket(ClientSocket);
	closesocket(ServerSocket);
	WSACleanup();

	return 0;
}