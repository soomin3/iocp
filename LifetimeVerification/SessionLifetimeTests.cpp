#include "pch.h"
#include "TestSupport.h"
#include "PacketDispatcher.h"
#include "Session.h"
#include "User.h"
#include "UserManager.h"
#include "IOCPManager.h"
#include <array>
#include <barrier>
#include <future>

namespace LifetimeVerification
{
	namespace
	{
		struct TestPacket
		{
			PacketHeader Header;
			uint32_t Sequence = 17;
			uint32_t Checksum = 17 ^ 0x57AF0031;
			TestPacket()
			{
				Header.packetSize = sizeof(*this);
				Header.packetID = PacketID::CS_CHAT_REQ;
			}
		};

		class PacketHandler final : public IPacketHandler
		{
		public:
			PacketHandler()
			{
				Entered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
				Continue = CreateEvent(nullptr, TRUE, FALSE, nullptr);
				Check(Entered && Continue, "create packet-handler gates");
			}
			~PacketHandler() { CloseHandle(Entered); CloseHandle(Continue); }
			bool Handle(const SPacketContext& Context, const char* Buffer, const PacketHeader& Header) override
			{
				LastSession.store(Context.Session);
				Check(Header.packetSize == sizeof(TestPacket), "received test packet has expected size");
				TestPacket Packet;
				memcpy(&Packet, Buffer, sizeof(Packet));
				Check(Packet.Checksum == (Packet.Sequence ^ 0x57AF0031), "receive buffer content is intact");
				if (Block)
				{
					SetEvent(Entered);
					Check(WaitForSingleObject(Continue, 5000) == WAIT_OBJECT_0, "release active receive continuation");
					Check(memcmp(&Packet, Buffer, sizeof(Packet)) == 0, "buffer survives disconnect during packet handling");
				}
				++Packets;
				return true;
			}
			bool Block = false;
			HANDLE Entered = nullptr;
			HANDLE Continue = nullptr;
			std::atomic_int Packets{ 0 };
			std::atomic<CSession*> LastSession{ nullptr };
		};

		struct Completion
		{
			SMyOverlapped* Operation = nullptr;
			DWORD Bytes = 0;
			DWORD Error = ERROR_SUCCESS;
		};

		class Fixture
		{
		public:
			Fixture() : Session(Dispatcher, 29)
			{
				Check(Dispatcher.Register(PacketID::CS_CHAT_REQ, Handler), "register lifetime packet handler");
				Listener = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
				Check(Listener != INVALID_SOCKET, "create loopback listener");
				Address.sin_family = AF_INET;
				Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				Check(::bind(Listener, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == 0, "bind ephemeral port");
				int Size = sizeof(Address);
				Check(getsockname(Listener, reinterpret_cast<sockaddr*>(&Address), &Size) == 0, "query ephemeral port");
				Check(listen(Listener, SOMAXCONN) == 0, "listen");
				Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 4);
				Check(Port != nullptr, "create four-worker IOCP");
				Check(CreateIoCompletionPort(reinterpret_cast<HANDLE>(Listener), Port, 0, 0) == Port, "associate listener");
				GUID Id = WSAID_ACCEPTEX;
				DWORD Bytes = 0;
				Check(WSAIoctl(Listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &Id, sizeof(Id),
					&g_AcceptEx, sizeof(g_AcceptEx), &Bytes, nullptr, nullptr) == 0, "load real AcceptEx");
			}
			~Fixture()
			{
				Check(Session.GetPendingIOCount() == 0, "all completions retired before fixture destruction");
				Session.Release();
				if (Client != INVALID_SOCKET) closesocket(Client);
				closesocket(Listener);
				CloseHandle(Port);
			}
			void Begin()
			{
				Check(Session.StartAccept(Listener, Port), "start real AcceptEx coroutine");
				Check(Session.GetPendingIOCount() == 1, "AcceptEx owns one completion pin");
			}
			void Connect()
			{
				Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				Check(Client != INVALID_SOCKET, "create loopback client");
				const int ReceiveBuffer = 1024;
				Check(setsockopt(Client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&ReceiveBuffer),
					sizeof(ReceiveBuffer)) == 0, "bound peer receive buffer");
				Check(connect(Client, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == 0, "connect loopback client");
			}
			void Accept()
			{
				Begin();
				Connect();
				const Completion Accepted = Next();
				Check(Accepted.Operation && Accepted.Operation->ioType == EIO_TYPE::ACCEPT &&
					Accepted.Error == ERROR_SUCCESS, "successful AcceptEx completion");
				Finish(Accepted);
				Check(Session.GetState() == ESessionState::CONNECTED && Session.GetPendingIOCount() == 1,
					"accepted session has one receive and an idle send coroutine");
			}
			Completion Next(DWORD Timeout = 5000)
			{
				DWORD Bytes = 0;
				ULONG_PTR Key = 0;
				OVERLAPPED* Overlapped = nullptr;
				const BOOL Success = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Overlapped, Timeout);
				const DWORD Error = Success ? ERROR_SUCCESS : GetLastError();
				if (!Overlapped)
				{
					Check(Error == WAIT_TIMEOUT, "empty completion poll must be a timeout");
					return {};
				}
				auto* Operation = reinterpret_cast<SMyOverlapped*>(Overlapped);
				Check(Operation->owner == &Session, "completion belongs to live fixture session");
				return { Operation, Bytes, Error };
			}
			void Resume(const Completion& Result)
			{
				Check(Result.Operation != nullptr, "completion arrives within deadline");
				bool Connected = false;
				switch (Result.Operation->ioType)
				{
				case EIO_TYPE::ACCEPT: Connected = Session.CompleteAccept(Result.Error); break;
				case EIO_TYPE::READ: Connected = Session.CompleteRead(Result.Bytes, Result.Error); break;
				case EIO_TYPE::WRITE: Connected = Session.CompleteWrite(Result.Bytes, Result.Error); break;
				case EIO_TYPE::SEND_WAKE: Connected = Session.ResumeSendLoop(); break;
				}
				if (!Connected) Session.Disconnect();
				Check(Session.GetPendingIOCount() > 0, "completion pin survives full coroutine resume");
			}
			void Finish(const Completion& Result)
			{
				Resume(Result);
				Check(Session.CompleteIO() >= 0, "retire each completion once");
			}
			void Drain()
			{
				const ULONGLONG Deadline = GetTickCount64() + 5000;
				while (Session.GetPendingIOCount() != 0)
				{
					Check(GetTickCount64() < Deadline, "all canceled operations drain within deadline");
					Finish(Next());
				}
			}
			void Reset()
			{
				Check(Session.TryReset(), "reset only after every resume and completion retires");
				Check(Session.GetState() == ESessionState::DISCONNECTED && Session.GetQueuedSendBytes() == 0,
					"recycled session releases queued buffers");
				Check(!Next(0).Operation, "no duplicate completion remains after recycling");
				if (Client != INVALID_SOCKET) { closesocket(Client); Client = INVALID_SOCKET; }
			}
			void Stop()
			{
				Session.Disconnect();
				Session.DetachUser();
				Drain();
				Reset();
			}
			void SendPacket()
			{
				const TestPacket Packet;
				Check(send(Client, reinterpret_cast<const char*>(&Packet), sizeof(Packet), 0) == sizeof(Packet),
					"send test packet");
			}
			CPacketDispatcher Dispatcher;
			PacketHandler Handler;
			CSession Session;
			SOCKET Client = INVALID_SOCKET;
			SOCKET Listener = INVALID_SOCKET;
			HANDLE Port = nullptr;
			sockaddr_in Address{};
		};

		void AcceptCancel()
		{
			Fixture Test;
			Test.Begin();
			Test.Session.Disconnect();
			Check(!Test.Session.TryReset(), "pending ACCEPT prevents premature frame destruction");
			Test.Drain();
			Test.Reset();
			Test.Begin();
			Test.Connect();
			const Completion Accepted = Test.Next();
			Check(Accepted.Operation && Accepted.Error == ERROR_SUCCESS, "dequeue successful ACCEPT before close");
			Test.Session.Disconnect();
			Test.Resume(Accepted);
			Check(!Test.Session.TryReset(), "returned ACCEPT resume is pinned until CompleteIO");
			Check(Test.Session.CompleteIO() == 0, "retire successful-but-closed accept");
			Check(Test.Session.GetGeneration() == 0, "closed accept does not create a connection generation");
			Test.Reset();
			Log("Verified canceled and already-successful ACCEPT; pending=0 before frame reclamation.");
		}

		void QueuedClose()
		{
			Fixture Test;
			Test.Accept();
			Test.SendPacket();
			const Completion Read = Test.Next();
			Check(Read.Operation && Read.Operation->ioType == EIO_TYPE::READ && Read.Error == ERROR_SUCCESS,
				"dequeue successful READ before concurrent close");
			Test.Session.Disconnect();
			Check(!Test.Session.TryReset(), "dequeued READ still pins receive buffer");
			Test.Resume(Read);
			Check(!Test.Session.TryReset(), "completed READ continuation stays pinned until worker retirement");
			Check(Test.Session.CompleteIO() == 0, "closed read loop posts no replacement receive");
			Check(Test.Handler.Packets == 1, "queued receive is processed exactly once");
			Test.Reset();
			Log("Verified dequeued READ, disconnect, resume, retirement and recycle ordering.");
		}

		void SendClose()
		{
			Fixture Test;
			Test.Accept();
			std::vector<char> Payload(512 * 1024, 'S');
			Check(Test.Session.Send(Payload.data(), Payload.size(), Test.Session.GetGeneration()), "enqueue send wake");
			const Completion Wake = Test.Next();
			Check(Wake.Operation && Wake.Operation->ioType == EIO_TYPE::SEND_WAKE, "dequeue send wake");
			Test.Session.Disconnect();
			Check(Test.Session.GetQueuedSendBytes() == Payload.size() && !Test.Session.TryReset(),
				"queued wake retains coroutine and owned payload until drained");
			Test.Finish(Wake);
			Test.Drain();
			Test.Reset();

			Test.Accept();
			const int NoBuffer = 0;
			Check(setsockopt(Test.Session.GetSocket(), SOL_SOCKET, SO_SNDBUF,
				reinterpret_cast<const char*>(&NoBuffer), sizeof(NoBuffer)) == 0, "disable send buffering");
			Check(Test.Session.Send(Payload.data(), Payload.size(), Test.Session.GetGeneration()), "enqueue large send");
			const Completion ActiveWake = Test.Next();
			Check(ActiveWake.Operation && ActiveWake.Operation->ioType == EIO_TYPE::SEND_WAKE, "resume actual send");
			Test.Finish(ActiveWake);
			Check(Test.Session.GetPendingIOCount() == 2, "READ and WRITE each hold an outstanding pin");
			Test.Session.Disconnect();
			Check(Test.Session.GetQueuedSendBytes() == Payload.size() && !Test.Session.TryReset(),
				"closing socket does not free kernel send buffer");
			Test.Drain();
			Test.Reset();
			Log("Verified SEND_WAKE and real WSASend cancellation; buffers cleared only after drain.");
		}

		void ResumeClose()
		{
			Fixture Test;
			Test.Handler.Block = true;
			Test.Accept();
			Test.SendPacket();
			std::thread Worker([&] { Test.Finish(Test.Next()); });
			Check(WaitForSingleObject(Test.Handler.Entered, 5000) == WAIT_OBJECT_0, "receive coroutine entered packet handler");
			Test.Session.Disconnect();
			Check(Test.Session.GetPendingIOCount() == 1 && !Test.Session.TryReset(),
				"active resume and its receive buffer survive concurrent disconnect");
			SetEvent(Test.Handler.Continue);
			Worker.join();
			Check(Test.Handler.Packets == 1 && Test.Session.GetPendingIOCount() == 0, "handler and resume finish exactly once");
			Test.Reset();
			Log("Verified close while packet handler is paused inside a live receive coroutine.");
		}

		void SessionReuse()
		{
			Fixture Test;
			Test.Accept();
			const uint64_t OldGeneration = Test.Session.GetGeneration();
			Test.Stop();
			Test.Begin();
			Check(!Test.Session.Disconnect(OldGeneration), "old generation must not cancel next pending ACCEPT");
			Check(Test.Session.GetState() == ESessionState::ACCEPTING, "next ACCEPT remains active");
			Test.Connect();
			Test.Finish(Test.Next());
			const uint64_t NewGeneration = Test.Session.GetGeneration();
			Check(NewGeneration == OldGeneration + 1, "successful reuse increments generation exactly once");
			auto User = std::make_shared<CUser>(73);
			Check(Test.Session.TryBeginLogin(NewGeneration) && Test.Session.CompleteLogin(User, NewGeneration),
				"attach user to new connection");
			Check(!Test.Session.Disconnect(OldGeneration), "stale callback cannot disconnect new connection");
			Check(!Test.Session.DetachUser(OldGeneration) && Test.Session.GetUser() == User,
				"stale callback cannot detach new connection user");
			Check(!Test.Session.Send("old", 3, OldGeneration), "stale send cannot publish data to reused connection");
			Test.SendPacket();
			Test.Finish(Test.Next());
			Check(Test.Handler.Packets == 1 && Test.Session.GetState() == ESessionState::CONNECTED,
				"new generation still receives after stale callback");
			Test.Stop();
			Log("Verified stale generation cannot cancel accepting/connected session or detach its new user.");
		}

		void ParallelClose()
		{
			Fixture Test;
			Test.Accept();
			const auto Generation = Test.Session.GetGeneration();
			const int NoBuffer = 0;
			Check(setsockopt(Test.Session.GetSocket(), SOL_SOCKET, SO_SNDBUF,
				reinterpret_cast<const char*>(&NoBuffer), sizeof(NoBuffer)) == 0, "disable parallel sender buffering");
			std::vector<char> InitialPayload(512 * 1024, 'P');
			Check(Test.Session.Send(InitialPayload.data(), InitialPayload.size(), Generation), "seed real send before parallel close");
			const Completion Wake = Test.Next();
			Check(Wake.Operation && Wake.Operation->ioType == EIO_TYPE::SEND_WAKE, "seed send wake");
			Test.Finish(Wake);
			Check(Test.Session.GetPendingIOCount() == 2, "READ and real WRITE posted before parallel workers start");
			std::atomic_bool StopWorkers{ false };
			std::array<std::thread, 4> Workers;
			for (auto& Worker : Workers)
			{
				Worker = std::thread([&]
				{
					while (!StopWorkers.load())
					{
						const Completion Result = Test.Next(10);
						if (Result.Operation) Test.Finish(Result);
					}
				});
			}
			std::barrier Gate(3);
			std::thread Sender([&]
			{
				std::array<char, 4096> Data{};
				Gate.arrive_and_wait();
				for (unsigned Index = 0; Index < 128; ++Index)
				{
					if (!Test.Session.Send(Data.data(), Data.size(), Generation)) break;
					SwitchToThread();
				}
			});
			std::thread Closer([&]
			{
				Gate.arrive_and_wait();
				Test.Session.Disconnect(Generation);
			});
			Test.SendPacket();
			Gate.arrive_and_wait();
			Sender.join();
			Closer.join();
			const ULONGLONG Deadline = GetTickCount64() + 5000;
			while (Test.Session.GetPendingIOCount() != 0)
			{
				Check(GetTickCount64() < Deadline, "four workers drain every completion after close");
				Sleep(1);
			}
			StopWorkers.store(true);
			for (auto& Worker : Workers) Worker.join();
			Test.Reset();
			Log("Verified four IOCP workers racing send/receive/disconnect; pending=0, no duplicate completion.");
		}

		void ManagerShutdown()
		{
			CPacketDispatcher Dispatcher;
			PacketHandler Handler;
			Handler.Block = true;
			Check(Dispatcher.Register(PacketID::CS_CHAT_REQ, Handler), "register manager shutdown handler");
			CUserManager Users;
			CIOCPManager Manager(Dispatcher, Users);
			Check(Manager.Init(), "GameServer IOCP manager init failed: ensure port 8694 is free; existing servers are not stopped");
			Check(Manager.Run(), "start production IOCP workers and session pool");
			const SOCKET Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			Check(Client != INVALID_SOCKET, "manager shutdown client socket");
			sockaddr_in Address{};
			Address.sin_family = AF_INET;
			Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			Address.sin_port = htons(8694);
			Check(connect(Client, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == 0, "connect to production IOCP manager");
			const TestPacket Packet;
			Check(send(Client, reinterpret_cast<const char*>(&Packet), sizeof(Packet), 0) == sizeof(Packet), "send shutdown gate packet");
			Check(WaitForSingleObject(Handler.Entered, 5000) == WAIT_OBJECT_0, "production IOCP worker entered receive handler");
			auto Stopped = std::async(std::launch::async, [&] { Manager.Stop(); });
			CSession* Session = Handler.LastSession.load();
			Check(Session != nullptr, "handler captured live session");
			const ULONGLONG Deadline = GetTickCount64() + 3000;
			while (Session->GetState() != ESessionState::DISCONNECTING)
			{
				Check(GetTickCount64() < Deadline, "manager published disconnect before draining");
				Sleep(1);
			}
			Check(Stopped.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout &&
				Session->GetPendingIOCount() > 0, "Stop waits for the active receive continuation");
			SetEvent(Handler.Continue);
			Check(Stopped.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
				"production Stop drains pending accepts/receive and joins IOCP workers");
			Stopped.get();
			Check(Session->GetPendingIOCount() == 0 && Handler.Packets == 1,
				"worker retirement precedes session pool destruction");
			Manager.PrintStatus();
			Manager.Stop();
			Manager.Release();
			closesocket(Client);
			Log("Verified production CIOCPManager Stop/drain/join/release with a paused receive handler and pending accepts.");
		}
	}

	void RunSessionLifetimeCase(std::string_view Id)
	{
		if (Id == "session_accept_cancel") AcceptCancel();
		else if (Id == "session_queued_close") QueuedClose();
		else if (Id == "session_send_close") SendClose();
		else if (Id == "session_resume_close") ResumeClose();
		else if (Id == "session_reuse") SessionReuse();
		else if (Id == "session_parallel_close") ParallelClose();
		else if (Id == "session_manager_shutdown") ManagerShutdown();
		else Check(false, "unknown session lifetime scenario");
	}
}
