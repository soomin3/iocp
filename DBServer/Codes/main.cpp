#include "OleDbDatabase.h"
#include "DBTransport.h"

#include <atomic>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	class CDBServer
	{
	public:
		~CDBServer()
		{
			Stop();
			if (m_WsaStarted)
			{
				WSACleanup();
			}
		}

		bool Start()
		{
			WSADATA Data{};
			if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
			{
				return false;
			}
			m_WsaStarted = true;
			m_ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (m_ListenSocket == INVALID_SOCKET)
			{
				return false;
			}
			const BOOL Exclusive = TRUE;
			sockaddr_in Address{};
			Address.sin_family = AF_INET;
			Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			Address.sin_port = htons(DBTransport::PORT);
			if (setsockopt(m_ListenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
				reinterpret_cast<const char*>(&Exclusive), sizeof(Exclusive)) == SOCKET_ERROR ||
				::bind(m_ListenSocket, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == SOCKET_ERROR ||
				listen(m_ListenSocket, SOMAXCONN) == SOCKET_ERROR)
			{
				return false;
			}
			std::promise<bool> Started;
			std::future<bool> Ready = Started.get_future();
			m_Running.store(true);
			try
			{
				m_Worker = std::thread(&CDBServer::Work, this, m_ListenSocket, std::move(Started));
			}
			catch (const std::system_error&)
			{
				m_Running.store(false);
				return false;
			}
			return Ready.get();
		}

		void Stop()
		{
			m_Running.store(false);
			{
				std::lock_guard<std::mutex> Lock(m_SocketLock);
				if (m_ListenSocket != INVALID_SOCKET)
				{
					closesocket(m_ListenSocket);
					m_ListenSocket = INVALID_SOCKET;
				}
				if (m_ClientSocket != INVALID_SOCKET)
				{
					shutdown(m_ClientSocket, SD_BOTH);
				}
			}
			if (m_Worker.joinable())
			{
				m_Worker.join();
			}
		}

		void PrintStatus()
		{
			std::lock_guard<std::mutex> Lock(m_SocketLock);
			std::cout << "running=" << (m_Running.load() ? "true" : "false")
				<< ", gameServerConnected=" << (m_ClientSocket != INVALID_SOCKET ? "true" : "false")
				<< ", requests=" << m_Requests.load() << ", responses=" << m_Responses.load() << '\n';
		}

	private:
		void Work(SOCKET _ListenSocket, std::promise<bool> _Started)
		{
			const HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (FAILED(Result))
			{
				m_Running.store(false);
				_Started.set_value(false);
				return;
			}
			{
				COleDbDatabase Database;
				const bool Connected = Database.Open();
				_Started.set_value(Connected);
				if (Connected)
				{
					Serve(_ListenSocket, Database);
				}
			}
			CoUninitialize();
			m_Running.store(false);
		}

		void Serve(SOCKET _ListenSocket, COleDbDatabase& _Database)
		{
			while (m_Running.load())
			{
				SOCKET Client = accept(_ListenSocket, nullptr, nullptr);
				if (Client == INVALID_SOCKET)
				{
					return;
				}
				{
					std::lock_guard<std::mutex> Lock(m_SocketLock);
					if (!m_Running.load())
					{
						closesocket(Client);
						return;
					}
					m_ClientSocket = Client;
				}
				if (DBTransport::ConfigureSocket(Client))
				{
					std::cout << "GameServer connected\n";
					ProcessRequests(Client, _Database);
				}
				{
					std::lock_guard<std::mutex> Lock(m_SocketLock);
					closesocket(Client);
					m_ClientSocket = INVALID_SOCKET;
				}
				std::cout << "GameServer disconnected\n";
			}
		}

		void ProcessRequests(SOCKET _Client, COleDbDatabase& _Database)
		{
			while (m_Running.load())
			{
				fd_set ReadSet;
				FD_ZERO(&ReadSet);
				FD_SET(_Client, &ReadSet);
				timeval Timeout{ 1, 0 };
				const int Ready = select(0, &ReadSet, nullptr, nullptr, &Timeout);
				if (Ready == 0)
				{
					continue;
				}
				PacketBuffer Request;
				if (Ready == SOCKET_ERROR || !DBTransport::ReceivePacket(_Client, Request, true))
				{
					return;
				}
				++m_Requests;
				PacketBuffer Response = _Database.Handle(Request);
				if (Response.empty() || !DBTransport::SendPacket(_Client, Response))
				{
					return;
				}
				++m_Responses;
			}
		}

		SOCKET m_ListenSocket = INVALID_SOCKET;
		SOCKET m_ClientSocket = INVALID_SOCKET;
		bool m_WsaStarted = false;
		std::mutex m_SocketLock;
		std::atomic_bool m_Running{ false };
		std::atomic_uint64_t m_Requests{ 0 };
		std::atomic_uint64_t m_Responses{ 0 };
		std::thread m_Worker;
	};
}

int main()
{
	CDBServer Server;
	if (!Server.Start())
	{
		std::cerr << "DBServer start failed\n";
		return 1;
	}
	std::cout << "DBServer is running on 127.0.0.1:" << DBTransport::PORT
		<< " (MSOLEDBSQL19)\nCommands: help, status, quit\n";
	std::string Command;
	while (getline(std::cin, Command))
	{
		if (Command == "quit")
		{
			break;
		}
		if (Command == "status")
		{
			Server.PrintStatus();
		}
		else if (Command == "help")
		{
			std::cout << "help - Show commands\nstatus - Show DBServer status\nquit - Stop DBServer\n";
		}
		else if (!Command.empty())
		{
			std::cout << "Unknown command: " << Command << '\n';
		}
	}
	Server.Stop();
	return 0;
}
