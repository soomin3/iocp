#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include "Config.h"
#include "Metrics.h"
#include "Packet.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>

using TClock = std::chrono::steady_clock;
using TTime = TClock::time_point;

enum class EOperation { Connect, Read, Write };
enum class EState { Closed, Connecting, Login, Nickname, Ready, Probe, Idle };
enum class EClose { Remote, Failure, Forced, Churn, ProbeDone, Stop };

struct SOperation
{
	OVERLAPPED Overlapped{};
	EOperation Type;
	bool Pending = false;
};

struct SClient
{
	size_t Index = 0;
	SOCKET Socket = INVALID_SOCKET;
	EState State = EState::Closed;
	SOperation Connect{ {}, EOperation::Connect };
	SOperation Read{ {}, EOperation::Read };
	SOperation Write{ {}, EOperation::Write };
	uint32_t Pending = 0;
	uint64_t ConnectCount = 0;
	uint64_t ChatSequence = 0;
	bool Connected = false;
	bool Covered = false;
	bool ChatPending = false;
	bool ProbeDelivered = false;
	size_t ProbeKind = 0;
	TTime NextConnect{};
	TTime ConnectStart{};
	TTime RequestStart{};
	TTime NextChat{};
	TTime Deadline{};
	std::array<char, 16384> ReceiveBuffer{};
	size_t Received = 0;
	PacketBuffer SendBuffer;
	size_t SendOffset = 0;
	std::string ChatToken;
};

class CLoadClient
{
public:
	explicit CLoadClient(SConfig _Config);
	~CLoadClient();
	int Run();
	static std::atomic_bool StopRequested;

private:
	void Initialize();
	void Tick(TTime _Now);
	void Connect(SClient& _Client, TTime _Now);
	void Close(SClient& _Client, EClose _Reason);
	void Drain();
	bool Poll(DWORD _WaitMs);
	void Complete(SClient& _Client, SOperation& _Operation, DWORD _Bytes, DWORD _Error);
	void RecordIOError(DWORD _Error);
	bool Read(SClient& _Client);
	bool Send(SClient& _Client, PacketBuffer _Packet);
	bool Write(SClient& _Client);
	bool ProcessPackets(SClient& _Client);
	bool HandlePacket(SClient& _Client, const char* _Data, const PacketHeader& _Header);
	void StartLogin(SClient& _Client);
	void StartProbe(SClient& _Client);
	void StartChat(SClient& _Client, TTime _Now);
	void MarkReady(SClient& _Client);
	void MarkCovered(SClient& _Client);
	void Report(TTime _Now, bool _Final);
	bool Succeeded() const;

	SConfig m_Config;
	SMetrics m_Metrics;
	SMetrics m_Previous;
	std::map<DWORD, uint64_t> m_IOErrors;
	std::vector<std::unique_ptr<SClient>> m_Clients;
	HANDLE m_Port = nullptr;
	LPFN_CONNECTEX m_ConnectEx = nullptr;
	bool m_WsaStarted = false;
	bool m_Stopping = false;
	uint64_t m_Pending = 0;
	uint64_t m_ProbeSequence = 0;
	size_t m_ConnectCursor = 0;
	size_t m_FaultCursor = 0;
	double m_ConnectTokens = 1;
	double m_ChurnTokens = 0;
	std::string m_RunToken;
	sockaddr_in m_RemoteAddress{};
	sockaddr_in m_LocalAddress{};
	TTime m_Start{};
	TTime m_LastTick{};
	TTime m_LastReport{};
	TTime m_NextFault{};
	std::ofstream m_Csv;
};
