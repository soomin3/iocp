#include "TestSupport.h"
#include "DBIocp.h"
#include "DBClient.h"
#include "DBServer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace LifetimeVerification
{
	namespace
	{
		struct SWinsock
		{
			SWinsock()
			{
				WSADATA Data{};
				Check(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "DB test WSAStartup");
			}
			~SWinsock() { WSACleanup(); }
		};

		void Close(SOCKET& Socket)
		{
			if (Socket != INVALID_SOCKET)
			{
				closesocket(Socket);
				Socket = INVALID_SOCKET;
			}
		}

		void Drain(DBIocp::CTask& Task, DBIocp::CContext& Context)
		{
			const ULONGLONG Deadline = GetTickCount64() + 3000;
			while (!Task.IsDone())
			{
				Check(GetTickCount64() < Deadline, "DB IOCP completion drain timed out");
				Context.RunOne(10);
			}
			Check(Context.PendingCount() == 0, "DB completion drain must release the pending operation");
		}

		DBIocp::CTask Capture(DBIocp::CTask Task, bool& Result)
		{
			Result = co_await std::move(Task);
			co_return true;
		}

		struct SPair
		{
			DBIocp::CContext ServerContext;
			DBIocp::CContext ClientContext;
			SOCKET Listener = INVALID_SOCKET;
			SOCKET Client = INVALID_SOCKET;
			SOCKET Server = INVALID_SOCKET;
			sockaddr_in Address{};

			SPair()
			{
				Check(ServerContext.Open() && ClientContext.Open(), "DB test completion ports");
				Listener = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
				Client = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
				Server = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
				Check(Listener != INVALID_SOCKET && Client != INVALID_SOCKET && Server != INVALID_SOCKET,
					"DB test socket allocation");
				Address.sin_family = AF_INET;
				Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				Check(::bind(Listener, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == 0,
					"DB test bind an ephemeral loopback port");
				int Size = sizeof(Address);
				Check(getsockname(Listener, reinterpret_cast<sockaddr*>(&Address), &Size) == 0,
					"DB test listener address");
				Check(listen(Listener, SOMAXCONN) == 0 && ServerContext.Associate(Listener),
					"DB test listener IOCP association");
			}

			void Connect()
			{
				bool Accepted = false;
				bool Connected = false;
				auto AcceptTask = Capture(DBIocp::Accept(ServerContext, Listener, Server), Accepted);
				auto ConnectTask = Capture(DBIocp::Connect(ClientContext, Client, Address, GetTickCount64() + 1000), Connected);
				AcceptTask.Resume();
				ConnectTask.Resume();
				const ULONGLONG Deadline = GetTickCount64() + 3000;
				while (!AcceptTask.IsDone() || !ConnectTask.IsDone())
				{
					Check(GetTickCount64() < Deadline, "DB test connection completion timed out");
					if (!AcceptTask.IsDone()) ServerContext.RunOne(1);
					if (!ConnectTask.IsDone()) ClientContext.RunOne(1);
				}
				Check(Accepted && Connected, "DB test AcceptEx/ConnectEx must connect");
			}

			~SPair()
			{
				Check(ServerContext.PendingCount() == 0 && ClientContext.PendingCount() == 0,
					"DB sockets cannot outlive undrained operation storage");
				Close(Client);
				Close(Server);
				Close(Listener);
			}
		};

		struct SObserved
		{
			unsigned Alive = 0;
			unsigned Destroyed = 0;
			unsigned Resumed = 0;
			DBIocp::SResult Result;
			std::array<char, 16> Received{};
		};

		struct SFrameScope
		{
			SObserved& Observed;
			explicit SFrameScope(SObserved& Value) : Observed(Value) { ++Observed.Alive; }
			~SFrameScope() { --Observed.Alive; ++Observed.Destroyed; }
		};

		DBIocp::CTask ReceiveOwned(DBIocp::CContext& Context, SOCKET Socket, SObserved& Observed, ULONGLONG Deadline = 0)
		{
			SFrameScope Scope(Observed);
			std::array<char, 16> Buffer{};
			Observed.Result = co_await DBIocp::Receive(Context, Socket, Buffer.data(), Buffer.size(), Deadline);
			++Observed.Resumed;
			Observed.Received = Buffer;
			co_return true;
		}

		DBIocp::CTask SendOwned(DBIocp::CContext& Context, SOCKET Socket, SObserved& Observed)
		{
			SFrameScope Scope(Observed);
			std::vector<char> Buffer(8 * 1024 * 1024, 'S');
			Observed.Result = co_await DBIocp::Send(Context, Socket, Buffer.data(), Buffer.size());
			++Observed.Resumed;
			co_return true;
		}

		void CheckAwaiting(const DBIocp::CTask& Task, const DBIocp::CContext& Context, const SObserved& Observed)
		{
			Check(!Task.IsDone() && Context.PendingCount() == 1 && Observed.Alive == 1 &&
				Observed.Destroyed == 0 && Observed.Resumed == 0,
				"DB cancellation must preserve coroutine locals and buffer until IOCP completion");
		}

		void CheckDrained(DBIocp::CContext& Context, SObserved& Observed, DWORD ExpectedError = ERROR_OPERATION_ABORTED)
		{
			Context.RunOne(0);
			Check(Observed.Alive == 0 && Observed.Destroyed == 1 && Observed.Resumed == 1,
				"DB completion must resume once and destroy coroutine locals once");
			Check(Observed.Result.Error == ExpectedError, "DB cancellation/timeout must take precedence over queued success");
		}

		void CancelConnect()
		{
			SPair Pair;
			bool Accepted = true;
			auto AcceptTask = Capture(DBIocp::Accept(Pair.ServerContext, Pair.Listener, Pair.Server), Accepted);
			AcceptTask.Resume();
			Check(Pair.ServerContext.PendingCount() == 1, "AcceptEx must be outstanding before cancellation");
			Pair.ServerContext.CancelAll();
			Close(Pair.Server);
			Close(Pair.Listener);
			Check(!AcceptTask.IsDone(), "closing AcceptEx sockets cannot destroy its suspended frame");
			Drain(AcceptTask, Pair.ServerContext);
			Check(!Accepted, "cancelled AcceptEx cannot publish an accepted connection");

			SPair ConnectPair;
			bool Connected = true;
			auto ConnectTask = Capture(DBIocp::Connect(ConnectPair.ClientContext, ConnectPair.Client,
				ConnectPair.Address, GetTickCount64() + 1000), Connected);
			ConnectTask.Resume();
			Check(ConnectPair.ClientContext.PendingCount() == 1, "ConnectEx must await IOCP completion");
			ConnectPair.ClientContext.CancelAll();
			Close(ConnectPair.Client);
			Check(!ConnectTask.IsDone(), "closing ConnectEx socket cannot destroy its suspended frame");
			Drain(ConnectTask, ConnectPair.ClientContext);
			Check(!Connected, "cancelled ConnectEx cannot publish a connected socket");
			Log("AcceptEx/ConnectEx cancellation drained after socket close; task frames remained alive.");
		}

		void CancelIo()
		{
			{
				SPair Pair;
				Pair.Connect();
				SObserved Observed;
				auto Task = ReceiveOwned(Pair.ServerContext, Pair.Server, Observed);
				Task.Resume();
				CheckAwaiting(Task, Pair.ServerContext, Observed);
				Pair.ServerContext.CancelAll();
				Close(Pair.Server);
				CheckAwaiting(Task, Pair.ServerContext, Observed);
				Drain(Task, Pair.ServerContext);
				CheckDrained(Pair.ServerContext, Observed);
			}
			{
				SPair Pair;
				Pair.Connect();
				const int SendBuffer = 0;
				const int ReceiveBuffer = 1024;
				Check(setsockopt(Pair.Client, SOL_SOCKET, SO_SNDBUF,
					reinterpret_cast<const char*>(&SendBuffer), sizeof(SendBuffer)) == 0 &&
					setsockopt(Pair.Server, SOL_SOCKET, SO_RCVBUF,
						reinterpret_cast<const char*>(&ReceiveBuffer), sizeof(ReceiveBuffer)) == 0,
					"DB test must limit socket buffers for an outstanding large send");
				SObserved Observed;
				auto Task = SendOwned(Pair.ClientContext, Pair.Client, Observed);
				Task.Resume();
				CheckAwaiting(Task, Pair.ClientContext, Observed);
				Pair.ClientContext.CancelAll();
				Close(Pair.Client);
				CheckAwaiting(Task, Pair.ClientContext, Observed);
				Drain(Task, Pair.ClientContext);
				CheckDrained(Pair.ClientContext, Observed);
			}
			Log("Cancelled receive and 8 MiB send retain coroutine-owned buffers until exactly one completion.");
		}

		void LateCompletion()
		{
			constexpr char Payload[] = "queued";
			for (const bool Expire : { false, true })
			{
				SPair Pair;
				Pair.Connect();
				Check(send(Pair.Client, Payload, sizeof(Payload), 0) == static_cast<int>(sizeof(Payload)), "DB test prefill receive");
				u_long Available = 0;
				const ULONGLONG Deadline = GetTickCount64() + 1000;
				while (Available < sizeof(Payload))
				{
					Check(GetTickCount64() < Deadline && ioctlsocket(Pair.Server, FIONREAD, &Available) == 0,
						"DB test prefilled receive bytes must arrive");
					SwitchToThread();
				}
				SObserved Observed;
				auto Task = ReceiveOwned(Pair.ServerContext, Pair.Server, Observed, Expire ? GetTickCount64() + 30 : 0);
				Task.Resume();
				CheckAwaiting(Task, Pair.ServerContext, Observed);
				if (Expire) Sleep(50);
				else
				{
					Pair.ServerContext.CancelAll();
					Close(Pair.Server);
				}
				CheckAwaiting(Task, Pair.ServerContext, Observed);
				Drain(Task, Pair.ServerContext);
				CheckDrained(Pair.ServerContext, Observed, Expire ? WSAETIMEDOUT : ERROR_OPERATION_ABORTED);
				Check(Observed.Result.Bytes == sizeof(Payload) &&
					memcmp(Observed.Received.data(), Payload, sizeof(Payload)) == 0,
					"a queued successful receive must preserve bytes when cancellation/timeout wins its result");
			}
			Log("Queued receive completions drained once: cancellation after close and timeout after the deadline.");
		}

		struct SBackendState
		{
			std::mutex Lock;
			std::condition_variable Ready;
			bool Entered = false;
			bool Release = false;
			bool Returned = false;
			bool RequestPreserved = false;
			bool HandlerDestroyed = false;
		};

		struct SHandlerLifetime
		{
			SBackendState& State;
			~SHandlerLifetime()
			{
				std::lock_guard<std::mutex> Guard(State.Lock);
				State.HandlerDestroyed = true;
			}
		};

		class CResponses final : public IPacketSender
		{
		public:
			bool SendPacket(PacketBuffer Packet) override
			{
				std::lock_guard<std::mutex> Guard(Lock);
				Packets.push_back(std::move(Packet));
				return true;
			}
			std::mutex Lock;
			std::vector<PacketBuffer> Packets;
		};

		void BackendShutdown()
		{
			SBackendState State;
			CDBServer Server;
			Check(Server.Start([&State]() -> CDBServer::RequestHandler
				{
					auto Lifetime = std::make_shared<SHandlerLifetime>(State);
					return [&State, Lifetime](const PacketBuffer& Request)
						{
							PacketBuffer Before = Request;
							{
								std::unique_lock<std::mutex> Guard(State.Lock);
								State.Entered = true;
								State.Ready.notify_all();
								Check(State.Ready.wait_for(Guard, std::chrono::seconds(5),
									[&State] { return State.Release; }), "mock DB release gate timed out");
								State.RequestPreserved = Request == Before;
								State.Returned = true;
							}
							SD_LOGIN_REQ Login;
							memcpy(&Login, Request.data(), sizeof(Login));
							DS_LOGIN_RES Response;
							Response.sessionID = Login.sessionID;
							Response.generation = Login.generation;
							Response.dbSucceeded = 1;
							return MakePacketBuffer(Response);
						};
				}),
				"mock DBServer start failed: ensure loopback port 8695 is free; existing servers are not stopped");

			CResponses Responses;
			CDBClient Client;
			Check(Client.StartWorker(Responses), "production DBClient must connect to the mock DBServer");
			SD_LOGIN_REQ Login;
			Login.sessionID = 73;
			Login.generation = 991;
			Check(Client.Submit(MakePacketBuffer(Login)), "production DBClient request submission");
			{
				std::unique_lock<std::mutex> Guard(State.Lock);
				Check(State.Ready.wait_for(Guard, std::chrono::seconds(3),
					[&State] { return State.Entered; }), "mock DB worker must enter the gated handler");
			}
			auto ClientStopped = std::async(std::launch::async, [&Client] { Client.StopWorker(); });
			Check(ClientStopped.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
				"DBClient must drain its receive cancellation while the DB backend is still blocked");
			ClientStopped.get();
			{
				std::lock_guard<std::mutex> Guard(Responses.Lock);
				Check(Responses.Packets.size() == 1 && Responses.Packets.front().size() == sizeof(DS_LOGIN_RES),
					"stopping DBClient must deliver one terminal response for the accepted request");
				DS_LOGIN_RES Response;
				memcpy(&Response, Responses.Packets.front().data(), sizeof(Response));
				Check(Response.sessionID == Login.sessionID && Response.generation == Login.generation &&
					Response.dbSucceeded == 0, "cancelled DB request must retain session/generation correlation");
			}

			auto ServerStopped = std::async(std::launch::async, [&Server] { Server.Stop(); });
			const ULONGLONG Deadline = GetTickCount64() + 1000;
			while (Server.IsRunning())
			{
				Check(GetTickCount64() < Deadline, "DBServer Stop must publish stopping state");
				SwitchToThread();
			}
			Check(ServerStopped.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
				"DBServer Stop must wait for the suspended database continuation");
			{
				std::lock_guard<std::mutex> Guard(State.Lock);
				Check(!State.Returned && !State.HandlerDestroyed,
					"DB handler and request must stay alive while network shutdown awaits its result");
				State.Release = true;
			}
			State.Ready.notify_all();
			Check(ServerStopped.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
				"DBServer must drain the late backend result and stop");
			ServerStopped.get();
			{
				std::lock_guard<std::mutex> Guard(State.Lock);
				Check(State.Returned && State.RequestPreserved && State.HandlerDestroyed,
					"late DB result must finish before handler and transport teardown");
			}
			Check(!Client.Submit(MakePacketBuffer(Login)), "stopped DBClient cannot accept more requests");
			Log("Production DBClient/DBServer shutdown preserved the late mock backend result and request lifetime.");
			Log("Scope: real loopback IOCP transport with a mock handler; SQL Server/OLE DB execution is not covered.");
		}
	}

	void RunDBLifetimeCase(std::string_view ID)
	{
		SWinsock Winsock;
		if (ID == "db_cancel_connect") CancelConnect();
		else if (ID == "db_cancel_io") CancelIo();
		else if (ID == "db_late_completion") LateCompletion();
		else if (ID == "db_backend_shutdown") BackendShutdown();
		else Check(false, "unknown DB lifetime test");
	}
}
