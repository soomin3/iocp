#include "TestSupport.h"
#include "AuthHandlers.h"
#include "ChatHandlers.h"
#include "ChatLogManager.h"
#include "DBClient.h"
#include "DBServer.h"
#include "IOCPManager.h"
#include "PacketDispatcher.h"
#include "UserManager.h"
#include "WorldChat.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace LifetimeVerification
{
	namespace
	{
		constexpr DWORD WaitMilliseconds = 3000;

		template<typename Predicate>
		void WaitUntil(Predicate Ready, const char* Message)
		{
			const ULONGLONG Deadline = GetTickCount64() + WaitMilliseconds;
			while (!Ready())
			{
				Check(GetTickCount64() < Deadline, Message);
				Sleep(1);
			}
		}

		template<size_t Size>
		void CopyText(char (&Target)[Size], std::string_view Text)
		{
			Check(Text.size() < Size, "test text must fit in the packet field");
			memset(Target, 0, Size);
			memcpy(Target, Text.data(), Text.size());
		}

		// Only the database execution is substituted. These responses travel through
		// the production DBServer, DBClient, IOCP dispatcher and authentication handlers.
		struct SMockDatabase
		{
			struct SAccount
			{
				std::string ID;
				int64_t AccountID;
				std::string Nickname;
			};
			std::array<SAccount, 4> Accounts{ {
				{ "alice", 1, "Alice" }, { "bob", 2, "Bob" },
				{ "newcomer", 3, "" }, { "second", 4, "" }
			} };
			std::atomic_uint32_t LoginCalls{ 0 };
			std::atomic_uint32_t NicknameCalls{ 0 };

			PacketBuffer Handle(const PacketBuffer& Buffer)
			{
				PacketHeader Header;
				Check(Buffer.size() >= sizeof(Header), "mock database received a complete header");
				memcpy(&Header, Buffer.data(), sizeof(Header));
				if (Header.packetID == PacketID::SD_LOGIN_REQ)
				{
					Check(Buffer.size() == sizeof(SD_LOGIN_REQ), "mock database login request size");
					++LoginCalls;
					SD_LOGIN_REQ Request;
					memcpy(&Request, Buffer.data(), sizeof(Request));
					DS_LOGIN_RES Response;
					Response.sessionID = Request.sessionID;
					Response.generation = Request.generation;
					Response.dbSucceeded = 1;
					Response.resultCode = 1;
					for (const SAccount& Account : Accounts)
					{
						if (Account.ID != Request.id || std::string_view(Request.pw) != "test-password") continue;
						Response.resultCode = 0;
						Response.accountID = Account.AccountID;
						Response.hasAccountID = 1;
						Response.hasNickname = Account.Nickname.empty() ? 0 : 1;
						CopyText(Response.nickname, Account.Nickname);
						break;
					}
					return MakePacketBuffer(Response);
				}

				Check(Header.packetID == PacketID::SD_CREATE_NICKNAME_REQ &&
					Buffer.size() == sizeof(SD_CREATE_NICKNAME_REQ), "mock database nickname request size");
				++NicknameCalls;
				SD_CREATE_NICKNAME_REQ Request;
				memcpy(&Request, Buffer.data(), sizeof(Request));
				DS_CREATE_NICKNAME_RES Response;
				Response.requestID = Request.requestID;
				Response.accountID = Request.accountID;
				Response.dbSucceeded = 1;
				Response.resultCode = 2;
				for (SAccount& Account : Accounts)
				{
					if (Account.AccountID != Request.accountID) continue;
					if (!Account.Nickname.empty())
					{
						Response.resultCode = 3;
						break;
					}
					bool Duplicated = false;
					for (const SAccount& Other : Accounts)
						if (Other.Nickname == Request.nickname) Duplicated = true;
					Response.resultCode = Duplicated ? 1 : 0;
					if (!Duplicated)
					{
						Account.Nickname = Request.nickname;
						CopyText(Response.nickname, Account.Nickname);
					}
					break;
				}
				return MakePacketBuffer(Response);
			}
		};

		// Pause a real DB completion immediately before its production handler.
		// Other IOCP workers continue accepting clients and handling login responses.
		class CGatedNicknameHandler final : public IPacketHandler
		{
		public:
			explicit CGatedNicknameHandler(CUserManager& Users) : Handler(Users) {}

			void Arm()
			{
				std::lock_guard<std::mutex> Guard(Lock);
				Armed = true;
				Entered = Released = Finished = false;
			}

			void WaitEntered()
			{
				std::unique_lock<std::mutex> Guard(Lock);
				Check(Ready.wait_for(Guard, std::chrono::milliseconds(WaitMilliseconds),
					[this] { return Entered; }), "late nickname response must reach the IOCP dispatch gate");
			}

			void ReleaseAndWait()
			{
				std::unique_lock<std::mutex> Guard(Lock);
				Released = true;
				Ready.notify_all();
				Check(Ready.wait_for(Guard, std::chrono::milliseconds(WaitMilliseconds),
					[this] { return Finished; }), "late nickname response handler must finish after gate release");
			}

			bool Handle(const SPacketContext& Context, const char* Buffer, const PacketHeader& Header) override
			{
				bool Gated = false;
				{
					std::unique_lock<std::mutex> Guard(Lock);
					Gated = Armed;
					if (Gated)
					{
						Armed = false;
						Entered = true;
						Ready.notify_all();
						Check(Ready.wait_for(Guard, std::chrono::seconds(8),
							[this] { return Released; }), "late nickname response gate release timed out");
					}
				}
				const bool Result = Handler.Handle(Context, Buffer, Header);
				if (Gated)
				{
					std::lock_guard<std::mutex> Guard(Lock);
					Finished = true;
					Ready.notify_all();
				}
				return Result;
			}

		private:
			CDSCreateNicknameHandler Handler;
			std::mutex Lock;
			std::condition_variable Ready;
			bool Armed = false;
			bool Entered = false;
			bool Released = false;
			bool Finished = false;
		};

		struct SGameServer
		{
			SMockDatabase Database;
			CDBServer DBServer;
			CDBClient DBClient;
			CUserManager Users;
			CChatLogManager Logs;
			CWorldChat World{ Users, Logs };
			CLoginHandler LoginHandler{ DBClient };
			CNickNameHandler NicknameHandler{ DBClient };
			CDSLoginHandler LoginResponseHandler{ Users };
			CGatedNicknameHandler NicknameResponseHandler{ Users };
			CChatHandlers ChatHandlers{ World };
			CPacketDispatcher Dispatcher;
			CIOCPManager Network{ Dispatcher, Users };

			SGameServer()
			{
				static std::atomic_uint32_t RunNumber{ 0 };
				const auto LogDirectory = std::filesystem::temp_directory_path() /
					("IOCP-GameVerification-" + std::to_string(GetCurrentProcessId())) /
					std::to_string(++RunNumber);
				Check(Logs.Start(LogDirectory), "game verification chat log worker must start");
				Check(Dispatcher.Register(PacketID::CS_LOGIN_REQ, LoginHandler) &&
					Dispatcher.Register(PacketID::CS_CREATE_NICKNAME_REQ, NicknameHandler) &&
					Dispatcher.Register(PacketID::DS_LOGIN_RES, LoginResponseHandler) &&
					Dispatcher.Register(PacketID::DS_CREATE_NICKNAME_RES, NicknameResponseHandler) &&
					ChatHandlers.RegisterAll(Dispatcher), "production game packet handlers must register");
				Check(Network.Init(),
					"GameServer test bind failed: port 8694 must be free; existing servers are not stopped");
				Check(DBServer.Start([this]() -> CDBServer::RequestHandler
					{
						return [this](const PacketBuffer& Request) { return Database.Handle(Request); };
					}), "DBServer test bind failed: port 8695 must be free; existing servers are not stopped");
				Check(DBClient.StartWorker(Network), "production GameServer DBClient must connect to mock-backed DBServer");
				Check(Network.Run(), "production GameServer IOCP workers and AcceptEx must start");
			}

			~SGameServer()
			{
				DBClient.StopWorker();
				Network.Stop();
				Logs.Stop();
				Network.Release();
				DBServer.Stop();
				Check(Users.GetUserCount() == 0, "GameServer shutdown must detach all authenticated users");
				Check(Logs.GetFailureCount() == 0, "game verification chat log writes must succeed");
			}
		};

		class CClient
		{
		public:
			CClient()
			{
				Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				Check(Socket != INVALID_SOCKET, "test client socket creation");
				u_long Nonblocking = 1;
				Check(ioctlsocket(Socket, FIONBIO, &Nonblocking) == 0, "test client nonblocking mode");
				const BOOL NoDelay = TRUE;
				Check(setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY,
					reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay)) == 0, "test client TCP_NODELAY");
				sockaddr_in Address{};
				Address.sin_family = AF_INET;
				Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				Address.sin_port = htons(8694);
				const int Result = connect(Socket, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address));
				if (Result != 0)
				{
					Check(WSAGetLastError() == WSAEWOULDBLOCK, "test client connect submission");
					Check(Wait(false, WaitMilliseconds), "test client connect timed out");
					int Error = 0;
					int Size = sizeof(Error);
					Check(getsockopt(Socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&Error), &Size) == 0 &&
						Error == 0, "test client must connect to GameServer");
				}
			}

			CClient(const CClient&) = delete;
			CClient& operator=(const CClient&) = delete;
			~CClient() { Close(); }

			void Close()
			{
				if (Socket == INVALID_SOCKET) return;
				const linger Abortive{ 1, 0 };
				setsockopt(Socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&Abortive), sizeof(Abortive));
				closesocket(Socket);
				Socket = INVALID_SOCKET;
			}

			void SendBytes(const char* Data, size_t Size)
			{
				const ULONGLONG Deadline = GetTickCount64() + WaitMilliseconds;
				size_t Offset = 0;
				while (Offset < Size)
				{
					Check(GetTickCount64() < Deadline, "test client send deadline");
					const int Sent = send(Socket, Data + Offset, static_cast<int>(Size - Offset), 0);
					if (Sent > 0) Offset += static_cast<size_t>(Sent);
					else
					{
						Check(Sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK,
							"test client send must succeed or await writable socket");
						Check(Wait(false, 50) || GetTickCount64() < Deadline, "test client writable wait");
					}
				}
			}

			template<typename Packet>
			void Send(const Packet& Value)
			{
				SendBytes(reinterpret_cast<const char*>(&Value), sizeof(Value));
			}

			template<typename Packet>
			Packet Receive()
			{
				Packet Value;
				const auto ExpectedID = Value.packetID;
				auto* Bytes = reinterpret_cast<char*>(&Value);
				ReceiveBytes(Bytes, sizeof(PacketHeader));
				Check(Value.packetSize == sizeof(Packet) && Value.packetID == ExpectedID,
					"GameServer response packet type and size must match expectation");
				ReceiveBytes(Bytes + sizeof(PacketHeader), sizeof(Packet) - sizeof(PacketHeader));
				return Value;
			}

			void ExpectQuiet(DWORD Milliseconds = 100)
			{
				Check(!Wait(true, Milliseconds),
					"client must stay connected without an unexpected response or broadcast");
			}

			void ExpectDisconnected()
			{
				Check(Wait(true, WaitMilliseconds), "invalid client packet must close the connection");
				char Byte = 0;
				const int Result = recv(Socket, &Byte, 1, 0);
				Check(Result == 0 || (Result == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET),
					"invalid client packet must be rejected without a response payload");
			}

		private:
			bool Wait(bool Read, DWORD Milliseconds)
			{
				fd_set Set;
				FD_ZERO(&Set);
				FD_SET(Socket, &Set);
				timeval Timeout{ static_cast<long>(Milliseconds / 1000), static_cast<long>((Milliseconds % 1000) * 1000) };
				const int Result = select(0, Read ? &Set : nullptr, Read ? nullptr : &Set, nullptr, &Timeout);
				Check(Result != SOCKET_ERROR, "test client select");
				return Result != 0;
			}

			void ReceiveBytes(char* Data, size_t Size)
			{
				const ULONGLONG Deadline = GetTickCount64() + WaitMilliseconds;
				size_t Offset = 0;
				while (Offset < Size)
				{
					const ULONGLONG Now = GetTickCount64();
					Check(Now < Deadline && Wait(true, static_cast<DWORD>(Deadline - Now)),
						"GameServer response timed out");
					const int Received = recv(Socket, Data + Offset, static_cast<int>(Size - Offset), 0);
					Check(Received > 0, "GameServer must not disconnect before the expected complete response");
					Offset += static_cast<size_t>(Received);
				}
			}

			SOCKET Socket = INVALID_SOCKET;
		};

		SC_LOGIN_RES Login(CClient& Client, std::string_view ID, std::string_view Password = "test-password")
		{
			CS_LOGIN_REQ Request;
			CopyText(Request.id, ID);
			CopyText(Request.pw, Password);
			Client.Send(Request);
			return Client.Receive<SC_LOGIN_RES>();
		}

		void ExpectLogin(CClient& Client, std::string_view ID, bool NeedNickname = false)
		{
			const auto Response = Login(Client, ID);
			Check(Response.result == ELoginResult::SUCCESS && Response.needNickname == (NeedNickname ? 1 : 0),
				"valid login must succeed and report the correct nickname requirement");
		}

		ENicknameResult Nickname(CClient& Client, std::string_view Name)
		{
			CS_CREATE_NICKNAME_REQ Request;
			CopyText(Request.nickname, Name);
			Client.Send(Request);
			return Client.Receive<SC_CREATE_NICKNAME_RES>().result;
		}

		void SendChat(CClient& Client, std::string_view Message)
		{
			CS_CHAT_REQ Request;
			CopyText(Request.message, Message);
			Client.Send(Request);
		}

		void LoginFlow()
		{
			SGameServer Server;
			CClient First;
			Check(Login(First, "", "").result == ELoginResult::INVALID_CREDENTIAL,
				"empty credentials must be rejected before reaching the DB");
			Check(Server.Database.LoginCalls == 0, "empty credentials cannot enqueue database work");
			Check(Login(First, "alice", "wrong-password").result == ELoginResult::INVALID_CREDENTIAL,
				"incorrect mock credentials must produce INVALID_CREDENTIAL");
			ExpectLogin(First, "alice");
			Check(Server.Users.GetUserCount() == 1, "successful login must register one active user");
			SActiveUserContext Original;
			Check(Server.Users.FindUser(1, Original), "first login must publish its authenticated user");
			CClient Duplicate;
			Check(Login(Duplicate, "alice").result == ELoginResult::ALREADY_LOGGED_IN,
				"a second session cannot authenticate the same active account");
			Check(Server.Users.GetUserCount() == 1, "duplicate login cannot replace the active user");
			SActiveUserContext AfterDuplicate;
			Check(Server.Users.FindUser(1, AfterDuplicate) && AfterDuplicate.User == Original.User &&
				AfterDuplicate.Session == Original.Session && AfterDuplicate.Generation == Original.Generation &&
				Original.Session->IsAuthenticated(Original.User, Original.Generation),
				"duplicate login must preserve the exact original account/session mapping");
			SendChat(First, "original login retained");
			Check(First.Receive<SC_CHAT_BROADCAST>().accountID == 1,
				"the original connection must remain usable after duplicate login rejection");
			Duplicate.ExpectQuiet();
			First.Close();
			WaitUntil([&] { return Server.Users.GetUserCount() == 0; }, "disconnect must remove the old authenticated user");
			ExpectLogin(Duplicate, "alice");
			Check(Server.Users.GetUserCount() == 1, "account must be available after its old connection closes");
			Log("Game login: empty/wrong credentials, duplicate account rejection and reconnect after disconnect passed.");
		}

		void NicknameFlow()
		{
			SGameServer Server;
			CClient NewUser;
			Check(Nickname(NewUser, "NewName") == ENicknameResult::INVALID_STATE,
				"nickname creation requires an authenticated user");
			ExpectLogin(NewUser, "newcomer", true);
			for (const std::string_view Invalid : { "", " leading", "trailing ", "abcdefghijklmnopq", "\xC3\x28" })
				Check(Nickname(NewUser, Invalid) == ENicknameResult::INVALID_NICKNAME,
					"empty, whitespace-boundary, overlong and invalid UTF-8 nicknames must be rejected");
			Check(Server.Database.NicknameCalls == 0, "invalid nickname requests cannot reach the database");
			Check(Nickname(NewUser, "Alice") == ENicknameResult::DUPLICATED,
				"mock database duplicate nickname result must reach the client");
			Check(Nickname(NewUser, "새사용자") == ENicknameResult::SUCCESS,
				"a valid UTF-8 nickname must be created");
			SActiveUserContext Context;
			Check(Server.Users.FindUser(3, Context) && Context.User->GetNickname() == "새사용자",
				"successful nickname creation must update the authenticated game user");
			Check(Nickname(NewUser, "ChangedName") == ENicknameResult::INVALID_STATE,
				"an already named user cannot submit another nickname creation");
			Check(Server.Database.NicknameCalls == 2, "nickname state rejection cannot enqueue a replacement write");
			NewUser.Close();
			WaitUntil([&] { return Server.Users.GetUserCount() == 0; }, "nickname user disconnect must finish");
			CClient Returning;
			ExpectLogin(Returning, "newcomer");
			Log("Game nickname: authentication, UTF-8/length validation, duplicate rejection, creation and immutable-name state passed.");
		}

		void ChatFlow()
		{
			SGameServer Server;
			CClient Alice;
			CClient Bob;
			CClient Unnamed;
			CClient Guest;
			ExpectLogin(Alice, "alice");
			ExpectLogin(Bob, "bob");
			ExpectLogin(Unnamed, "newcomer", true);
			for (CClient* NotReady : { &Guest, &Unnamed })
			{
				SendChat(*NotReady, "not allowed");
				Check(NotReady->Receive<SC_CHAT_ERROR>().error == EChatError::NOT_READY,
					"chat requires authentication and an assigned nickname");
			}
			const std::string Message = "안녕하세요 \"chat\"";
			SendChat(Alice, Message);
			const auto Sender = Alice.Receive<SC_CHAT_BROADCAST>();
			const auto Other = Bob.Receive<SC_CHAT_BROADCAST>();
			Check(Sender.sequence != 0 && Sender.sequence == Other.sequence &&
				Sender.utcMilliseconds > 0 && Sender.utcMilliseconds == Other.utcMilliseconds &&
				Sender.accountID == 1 && Other.accountID == 1 &&
				std::string_view(Sender.nickname) == "Alice" && std::string_view(Other.nickname) == "Alice" &&
				std::string_view(Sender.message) == Message && std::string_view(Other.message) == Message,
				"all eligible users must receive the same sender, UTF-8 text, sequence and timestamp");
			Guest.ExpectQuiet();
			Unnamed.ExpectQuiet();
			SendChat(Bob, "");
			Check(Bob.Receive<SC_CHAT_ERROR>().error == EChatError::INVALID_MESSAGE,
				"empty chat must be rejected without broadcast");
			Alice.ExpectQuiet();
			WaitUntil([&] { return Server.Logs.GetWrittenCount() == 1; }, "accepted world chat must reach the log worker");
			Check(Server.World.GetProcessedCount() == 1 && Server.World.GetAudienceCount() == 2,
				"world chat counters and audience must match eligible game users");
			Log("Game chat: readiness, sender/content consistency, eligible broadcast audience, invalid text and audit write passed.");
		}

		void ChatLimit()
		{
			SGameServer Server;
			CClient Receiver;
			CClient Sender;
			ExpectLogin(Receiver, "bob");
			ExpectLogin(Sender, "alice");
			PacketBuffer Batch;
			for (int Index = 0; Index < 6; ++Index)
			{
				CS_CHAT_REQ Request;
				CopyText(Request.message, "message-" + std::to_string(Index));
				const PacketBuffer Bytes = MakePacketBuffer(Request);
				Batch.insert(Batch.end(), Bytes.begin(), Bytes.end());
			}
			Sender.SendBytes(Batch.data(), Batch.size());
			uint64_t LastSequence = 0;
			for (int Index = 0; Index < 5; ++Index)
			{
				const auto Sent = Sender.Receive<SC_CHAT_BROADCAST>();
				const auto Received = Receiver.Receive<SC_CHAT_BROADCAST>();
				Check(Sent.sequence > LastSequence && Received.sequence == Sent.sequence &&
					std::string_view(Received.message) == "message-" + std::to_string(Index),
					"the first five chat messages must broadcast once and in sequence");
				LastSequence = Sent.sequence;
			}
			Check(Sender.Receive<SC_CHAT_ERROR>().error == EChatError::RATE_LIMITED,
				"the sixth message in the same second must receive RATE_LIMITED");
			Receiver.ExpectQuiet();
			Check(Server.World.GetProcessedCount() == 5, "rate-limited chat cannot enter world broadcast");
			WaitUntil([&] { return Server.Logs.GetWrittenCount() == 5; }, "only accepted chat messages may enter the audit log");
			Log("Game chat quota: five broadcasts allowed, sixth rejected, no extra audience delivery or audit record.");
		}

		void StaleResponse()
		{
			SGameServer Server;
			CClient OldConnection;
			ExpectLogin(OldConnection, "newcomer", true);
			SActiveUserContext OldContext;
			Check(Server.Users.FindUser(3, OldContext), "old authenticated game user must exist");

			// Fill the other slots so the replacement connection must reuse this
			// exact CSession object, rather than merely allocate another pool entry.
			std::vector<std::unique_ptr<CClient>> Occupied;
			for (int Index = 1; Index < USER_SIZE; ++Index)
			{
				auto Client = std::make_unique<CClient>();
				Check(Nickname(*Client, "Probe") == ENicknameResult::INVALID_STATE,
					"every other session slot must accept and dispatch its probe");
				Occupied.push_back(std::move(Client));
			}

			Server.NicknameResponseHandler.Arm();
			CS_CREATE_NICKNAME_REQ Request;
			CopyText(Request.nickname, "AfterReconnect");
			OldConnection.Send(Request);
			Server.NicknameResponseHandler.WaitEntered();
			Check(Nickname(OldConnection, "WhilePending") == ENicknameResult::INVALID_STATE &&
				Server.Database.NicknameCalls == 1,
				"a pending nickname request cannot be replaced by a second database write");
			OldConnection.Close();
			WaitUntil([&] { return Server.Users.GetUserCount() == 0; },
				"disconnect must remove the user while its nickname response is gated");
			CClient Replacement;
			ExpectLogin(Replacement, "newcomer");
			SActiveUserContext NewContext;
			Check(Server.Users.FindUser(3, NewContext) && NewContext.User != OldContext.User &&
				NewContext.Session == OldContext.Session && NewContext.Generation != OldContext.Generation,
				"relogin must reuse the exact session with a new generation and a distinct user");
			Server.NicknameResponseHandler.ReleaseAndWait();
			Replacement.ExpectQuiet();
			Check(NewContext.Session->IsAuthenticated(NewContext.User, NewContext.Generation) &&
				NewContext.User->GetNickname() == "AfterReconnect" && Server.Users.GetUserCount() == 1,
				"late nickname response cannot complete, detach or overwrite the replacement user");
			SendChat(Replacement, "still connected");
			Check(Replacement.Receive<SC_CHAT_BROADCAST>().accountID == 3,
				"replacement connection must remain usable after the old DB response finishes");
			Log("Game stale response: real queued nickname reply ignored after account relogin and exact session-slot reuse.");
		}

		void ProtocolBoundary()
		{
			SGameServer Server;
			CClient Valid;
			CS_LOGIN_REQ EmptyLogin;
			const auto* Bytes = reinterpret_cast<const char*>(&EmptyLogin);
			Valid.SendBytes(Bytes, 1);
			Valid.ExpectQuiet(30);
			Valid.SendBytes(Bytes + 1, sizeof(EmptyLogin) - 1);
			Check(Valid.Receive<SC_LOGIN_RES>().result == ELoginResult::INVALID_CREDENTIAL,
				"split game packet must be assembled before dispatch");
			CS_CREATE_NICKNAME_REQ NicknameRequest;
			CopyText(NicknameRequest.nickname, "Probe");
			PacketBuffer Joined = MakePacketBuffer(NicknameRequest);
			const PacketBuffer Second = MakePacketBuffer(NicknameRequest);
			Joined.insert(Joined.end(), Second.begin(), Second.end());
			Valid.SendBytes(Joined.data(), Joined.size());
			for (int Index = 0; Index < 2; ++Index)
				Check(Valid.Receive<SC_CREATE_NICKNAME_RES>().result == ENicknameResult::INVALID_STATE,
					"coalesced game packets must each dispatch exactly once");

			{
				CClient Attacker;
				SD_LOGIN_REQ ForgedRequest;
				Attacker.Send(ForgedRequest);
				Attacker.ExpectDisconnected();
			}
			{
				CClient Attacker;
				DS_LOGIN_RES ForgedResponse;
				ForgedResponse.dbSucceeded = ForgedResponse.hasAccountID = 1;
				ForgedResponse.accountID = 1;
				ForgedResponse.resultCode = 0;
				Attacker.Send(ForgedResponse);
				Attacker.ExpectDisconnected();
			}
			for (const uint16_t InvalidSize : { uint16_t{ 1 }, uint16_t{ sizeof(PacketHeader) } })
			{
				CClient Malformed;
				PacketHeader Header;
				Header.packetID = PacketID::CS_LOGIN_REQ;
				Header.packetSize = InvalidSize;
				Malformed.Send(Header);
				Malformed.ExpectDisconnected();
			}
			Check(Server.Database.LoginCalls == 0 && Server.Users.GetUserCount() == 0,
				"malformed and internal-only client packets cannot cause DB login or authentication");
			ExpectLogin(Valid, "alice");
			Log("Game protocol: split/combined requests, forged SD/DS packets and invalid sizes passed; healthy client remained usable.");
		}
	}

	void RunGameServerCase(std::string_view Id)
	{
		Log("Scope: production GameServer/DBServer IOCP and game handlers; database execution uses deterministic mock accounts.");
		Log("These cases do not execute SQL Server/OLE DB or cover game features absent from this repository.");
		if (Id == "game_login") LoginFlow();
		else if (Id == "game_nickname") NicknameFlow();
		else if (Id == "game_chat") ChatFlow();
		else if (Id == "game_chat_limit") ChatLimit();
		else if (Id == "game_stale_response") StaleResponse();
		else if (Id == "game_protocol") ProtocolBoundary();
		else Check(false, "unknown GameServer test");
	}
}
