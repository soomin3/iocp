#include "LoadClient.h"
#include <cassert>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace std::chrono;

std::atomic_bool CLoadClient::StopRequested = false;

namespace
{
	uint64_t Microseconds(TTime _Start, TTime _End)
	{
		return static_cast<uint64_t>(duration_cast<microseconds>(_End - _Start).count());
	}
	BOOL WINAPI ConsoleHandler(DWORD _Event)
	{
		if (_Event != CTRL_C_EVENT && _Event != CTRL_BREAK_EVENT) return FALSE;
		CLoadClient::StopRequested.store(true);
		return TRUE;
	}
	template<typename T>
	T Decode(const char* _Data)
	{
		T Packet;
		memcpy(&Packet, _Data, sizeof(Packet));
		return Packet;
	}
}

CLoadClient::CLoadClient(SConfig _Config) : m_Config(std::move(_Config))
{
}

CLoadClient::~CLoadClient()
{
	Drain();
	if (m_Port != nullptr) CloseHandle(m_Port);
	if (m_WsaStarted) WSACleanup();
	SetConsoleCtrlHandler(ConsoleHandler, FALSE);
}

void CLoadClient::Initialize()
{
	WSADATA Data{};
	if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
		throw std::runtime_error("WSAStartup failed");
	m_WsaStarted = true;
	addrinfo Hints{};
	Hints.ai_family = AF_INET;
	Hints.ai_socktype = SOCK_STREAM;
	Hints.ai_protocol = IPPROTO_TCP;
	addrinfo* Addresses = nullptr;
	const std::string Port = std::to_string(m_Config.Port);
	const int Result = getaddrinfo(m_Config.Host.c_str(), Port.c_str(), &Hints, &Addresses);
	if (Result != 0) throw std::runtime_error("IPv4 host resolution failed: " + std::to_string(Result));
	memcpy(&m_RemoteAddress, Addresses->ai_addr, sizeof(m_RemoteAddress));
	freeaddrinfo(Addresses);
	m_LocalAddress.sin_family = AF_INET;
	if (InetPtonA(AF_INET, m_Config.LocalIP.c_str(), &m_LocalAddress.sin_addr) != 1)
		throw std::runtime_error("--local-ip must be an IPv4 address");
	m_Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
	if (m_Port == nullptr) throw std::runtime_error("CreateIoCompletionPort failed");
	SOCKET Probe = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (Probe == INVALID_SOCKET) throw std::runtime_error("WSASocket failed");
	GUID Guid = WSAID_CONNECTEX;
	DWORD Bytes = 0;
	const int ExtensionResult = WSAIoctl(Probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid),
		&m_ConnectEx, sizeof(m_ConnectEx), &Bytes, nullptr, nullptr);
	closesocket(Probe);
	if (ExtensionResult == SOCKET_ERROR) throw std::runtime_error("ConnectEx lookup failed");
	if (!m_Config.CsvPath.empty())
	{
		const std::filesystem::path Path(m_Config.CsvPath);
		if (Path.has_parent_path()) std::filesystem::create_directories(Path.parent_path());
		m_Csv.open(Path, std::ios::out | std::ios::trunc);
		if (!m_Csv) throw std::runtime_error("Cannot open CSV output");
		m_Csv << "row,elapsed_s,connected,ready,peak_connected,peak_ready,covered_clients,max_tick_gap_ms,"
			"connects_per_s,login_tps,chat_tps,tx_packets_per_s,rx_packets_per_s,tx_bytes_per_s,rx_bytes_per_s";
		for (const auto Name : CounterNames) m_Csv << ',' << Name;
		for (const auto Name : LatencyNames)
			m_Csv << ',' << Name << "_samples," << Name << "_avg_ms," << Name << "_p95_ms," << Name << "_p99_ms";
		for (size_t Index = 0; Index < m_Metrics.LoginResults.size(); ++Index) m_Csv << ",login_result_" << Index;
		for (size_t Index = 0; Index < m_Metrics.NicknameResults.size(); ++Index) m_Csv << ",nickname_result_" << Index;
		for (size_t Index = 0; Index < m_Metrics.ChatResults.size(); ++Index) m_Csv << ",chat_error_" << Index;
		for (const auto Name : ProbeNames) m_Csv << ',' << Name << "_sent," << Name << "_passed";
		m_Csv << '\n';
	}
	m_Clients.reserve(m_Config.Clients);
	for (size_t Index = 0; Index < m_Config.Clients; ++Index)
	{
		auto Client = std::make_unique<SClient>();
		Client->Index = Index;
		m_Clients.push_back(std::move(Client));
	}
	m_RunToken = "dc:" + std::to_string(GetCurrentProcessId()) + ":" +
		std::to_string(TClock::now().time_since_epoch().count()) + ":";
	StopRequested.store(false);
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
}

int CLoadClient::Run()
{
	Initialize();
	m_Start = m_LastTick = m_LastReport = TClock::now();
	m_NextFault = m_Start + milliseconds(m_Config.FaultEveryMs);
	std::cout << "scenario=" << m_Config.Scenario << " target=" << m_Config.Host << ':' << m_Config.Port
		<< " clients=" << m_Config.Clients << " duration_s=" << m_Config.DurationSeconds
		<< " connect_rate=" << m_Config.ConnectRate << " active_percent=" << m_Config.ActivePercent
		<< " interval_ms=" << m_Config.IntervalMs << '\n';
	const TTime End = m_Start + seconds(m_Config.DurationSeconds);
	while (!StopRequested.load())
	{
		const TTime Now = TClock::now();
		if (Now >= End) break;
		if (Now - m_LastTick >= milliseconds(10)) Tick(Now);
		if (Now - m_LastReport >= milliseconds(m_Config.ReportMs)) Report(Now, false);
		// Bound completion work so timers/faults cannot starve under broadcast load.
		Poll(1);
		for (int Index = 0; Index < 255 && Poll(0); ++Index) {}
	}
	const TTime Finished = TClock::now();
	const bool Success = Succeeded();
	// Final gauges represent the workload before teardown. Cancellations are still recorded.
	const uint64_t Connected = m_Metrics.Connected;
	const uint64_t Ready = m_Metrics.Ready;
	Drain();
	m_Metrics.Connected = Connected;
	m_Metrics.Ready = Ready;
	Report(Finished, true);
	std::cout << "result=" << (Success ? "PASS" : "FAIL") << " stopped=" << StopRequested.load()
		<< " covered=" << m_Metrics.CoveredClients << '/' << m_Config.Clients << " pending_io=" << m_Pending << '\n';
	return StopRequested.load() ? 130 : (Success ? 0 : 1);
}

void CLoadClient::RecordIOError(DWORD _Error)
{
	++m_Metrics[ECounter::IOErrors];
	++m_IOErrors[_Error];
}

void CLoadClient::Connect(SClient& _Client, TTime _Now)
{
	assert(_Client.Pending == 0 && _Client.Socket == INVALID_SOCKET);
	++m_Metrics[ECounter::Attempts];
	_Client.State = EState::Connecting;
	_Client.ConnectStart = _Now;
	_Client.Deadline = _Now + milliseconds(m_Config.TimeoutMs);
	_Client.Received = 0;
	_Client.SendBuffer.clear();
	_Client.SendOffset = 0;
	_Client.ChatToken.clear();
	_Client.ChatPending = false;
	_Client.ProbeDelivered = false;
	_Client.Socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (_Client.Socket == INVALID_SOCKET)
	{
		RecordIOError(static_cast<DWORD>(WSAGetLastError()));
		++m_Metrics[ECounter::ConnectFailures];
		Close(_Client, EClose::Failure);
		return;
	}
	BOOL NoDelay = TRUE;
	if (bind(_Client.Socket, reinterpret_cast<const sockaddr*>(&m_LocalAddress), sizeof(m_LocalAddress)) == SOCKET_ERROR ||
		setsockopt(_Client.Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay)) == SOCKET_ERROR)
	{
		RecordIOError(static_cast<DWORD>(WSAGetLastError()));
		++m_Metrics[ECounter::ConnectFailures];
		Close(_Client, EClose::Failure);
		return;
	}
	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(_Client.Socket), m_Port, reinterpret_cast<ULONG_PTR>(&_Client), 0) == nullptr)
	{
		RecordIOError(GetLastError());
		++m_Metrics[ECounter::ConnectFailures];
		Close(_Client, EClose::Failure);
		return;
	}
	_Client.Connect.Overlapped = {};
	_Client.Connect.Pending = true;
	++_Client.Pending;
	++m_Pending;
	if (!m_ConnectEx(_Client.Socket, reinterpret_cast<const sockaddr*>(&m_RemoteAddress), sizeof(m_RemoteAddress),
		nullptr, 0, nullptr, &_Client.Connect.Overlapped))
	{
		const DWORD Error = static_cast<DWORD>(WSAGetLastError());
		if (Error != ERROR_IO_PENDING)
		{
			_Client.Connect.Pending = false;
			--_Client.Pending;
			--m_Pending;
			RecordIOError(Error);
			++m_Metrics[ECounter::ConnectFailures];
			Close(_Client, EClose::Failure);
		}
	}
}

void CLoadClient::Close(SClient& _Client, EClose _Reason)
{
	if (_Client.State == EState::Closed) return;
	const bool HasRequest = _Client.State == EState::Login || _Client.State == EState::Nickname || _Client.ChatPending;
	if (HasRequest)
		++m_Metrics[_Reason == EClose::Stop ? ECounter::ShutdownCanceled : ECounter::AbandonedRequests];
	if (_Client.State == EState::Ready) --m_Metrics.Ready;
	if (_Client.Connected)
	{
		--m_Metrics.Connected;
		if (_Reason != EClose::Stop) ++m_Metrics[ECounter::Disconnects];
		if (_Reason == EClose::Remote) ++m_Metrics[ECounter::UnexpectedDisconnects];
	}
	_Client.Connected = false;
	_Client.ChatPending = false;
	_Client.State = EState::Closed;
	_Client.NextConnect = TClock::now() + milliseconds(m_Config.ReconnectDelayMs);
	if (_Client.Socket != INVALID_SOCKET)
	{
		if (_Reason == EClose::Forced || _Reason == EClose::Stop || _Reason == EClose::Failure)
		{
			linger Linger{ 1, 0 };
			setsockopt(_Client.Socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&Linger), sizeof(Linger));
		}
		else shutdown(_Client.Socket, SD_SEND);
		closesocket(_Client.Socket);
		_Client.Socket = INVALID_SOCKET;
	}
	// OVERLAPPED and both buffers remain alive and untouched until Pending becomes zero.
}

void CLoadClient::Drain()
{
	if (m_Stopping) return;
	m_Stopping = true;
	for (auto& Client : m_Clients) Close(*Client, EClose::Stop);
	while (m_Pending != 0) Poll(100);
}

bool CLoadClient::Poll(DWORD _WaitMs)
{
	DWORD Bytes = 0;
	ULONG_PTR Key = 0;
	OVERLAPPED* Overlapped = nullptr;
	const BOOL Success = GetQueuedCompletionStatus(m_Port, &Bytes, &Key, &Overlapped, _WaitMs);
	const DWORD Error = Success ? ERROR_SUCCESS : GetLastError();
	if (Overlapped == nullptr) return false;
	auto& Client = *reinterpret_cast<SClient*>(Key);
	auto& Operation = *reinterpret_cast<SOperation*>(Overlapped);
	assert(Operation.Pending && Client.Pending > 0 && m_Pending > 0);
	Operation.Pending = false;
	--Client.Pending;
	--m_Pending;
	if (Client.State != EState::Closed) Complete(Client, Operation, Bytes, Error);
	return true;
}

bool CLoadClient::Read(SClient& _Client)
{
	assert(!_Client.Read.Pending && _Client.Received < _Client.ReceiveBuffer.size());
	WSABUF Buffer{ static_cast<ULONG>(_Client.ReceiveBuffer.size() - _Client.Received), _Client.ReceiveBuffer.data() + _Client.Received };
	_Client.Read.Overlapped = {};
	_Client.Read.Pending = true;
	++_Client.Pending;
	++m_Pending;
	DWORD Flags = 0;
	if (WSARecv(_Client.Socket, &Buffer, 1, nullptr, &Flags, &_Client.Read.Overlapped, nullptr) == SOCKET_ERROR)
	{
		const DWORD Error = static_cast<DWORD>(WSAGetLastError());
		if (Error != WSA_IO_PENDING)
		{
			_Client.Read.Pending = false;
			--_Client.Pending;
			--m_Pending;
			Complete(_Client, _Client.Read, 0, Error);
			return false;
		}
	}
	return true;
}

bool CLoadClient::Send(SClient& _Client, PacketBuffer _Packet)
{
	assert(!_Client.Write.Pending && _Client.SendBuffer.empty());
	_Client.SendBuffer = std::move(_Packet);
	_Client.SendOffset = 0;
	return Write(_Client);
}

bool CLoadClient::Write(SClient& _Client)
{
	WSABUF Buffer{ static_cast<ULONG>(_Client.SendBuffer.size() - _Client.SendOffset), _Client.SendBuffer.data() + _Client.SendOffset };
	_Client.Write.Overlapped = {};
	_Client.Write.Pending = true;
	++_Client.Pending;
	++m_Pending;
	if (WSASend(_Client.Socket, &Buffer, 1, nullptr, 0, &_Client.Write.Overlapped, nullptr) == SOCKET_ERROR)
	{
		const DWORD Error = static_cast<DWORD>(WSAGetLastError());
		if (Error != WSA_IO_PENDING)
		{
			_Client.Write.Pending = false;
			--_Client.Pending;
			--m_Pending;
			RecordIOError(Error);
			Close(_Client, EClose::Remote);
			return false;
		}
	}
	return true;
}

void CLoadClient::Complete(SClient& _Client, SOperation& _Operation, DWORD _Bytes, DWORD _Error)
{
	if (_Error != ERROR_SUCCESS || (_Operation.Type != EOperation::Connect && _Bytes == 0))
	{
		if (_Operation.Type == EOperation::Connect)
		{
			++m_Metrics[ECounter::ConnectFailures];
			RecordIOError(_Error);
			Close(_Client, EClose::Failure);
		}
		else if (_Client.State == EState::Probe && _Client.ProbeKind != 4 && _Client.ProbeDelivered &&
			_Operation.Type == EOperation::Read && (_Error == ERROR_SUCCESS || _Error == WSAECONNRESET || _Error == WSAECONNABORTED || _Error == ERROR_NETNAME_DELETED))
		{
			++m_Metrics[ECounter::ProbeClosed];
			++m_Metrics.ProbePassed[_Client.ProbeKind];
			MarkCovered(_Client);
			Close(_Client, EClose::ProbeDone);
		}
		else
		{
			if (_Client.State == EState::Probe) ++m_Metrics[ECounter::ProbeUnexpected];
			if (_Error != ERROR_SUCCESS) RecordIOError(_Error);
			Close(_Client, EClose::Remote);
		}
		return;
	}
	if (_Operation.Type == EOperation::Connect)
	{
		if (setsockopt(_Client.Socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == SOCKET_ERROR)
		{
			++m_Metrics[ECounter::ConnectFailures];
			RecordIOError(static_cast<DWORD>(WSAGetLastError()));
			Close(_Client, EClose::Failure);
			return;
		}
		_Client.Connected = true;
		++m_Metrics.Connected;
		m_Metrics.PeakConnected = std::max(m_Metrics.PeakConnected, m_Metrics.Connected);
		++m_Metrics[ECounter::Connects];
		if (_Client.ConnectCount++ != 0) ++m_Metrics[ECounter::Reconnects];
		m_Metrics.Latencies[0].Add(Microseconds(_Client.ConnectStart, TClock::now()));
		_Client.State = EState::Idle;
		if (m_Config.Scenario == "connect-only") MarkCovered(_Client);
		// Probes post their receive after the send completion to distinguish delivery from early EOF.
		if (m_Config.IsProbe(_Client.Index)) StartProbe(_Client);
		else if (Read(_Client) && m_Config.Scenario != "connect-only") StartLogin(_Client);
	}
	else if (_Operation.Type == EOperation::Write)
	{
		m_Metrics[ECounter::TxBytes] += _Bytes;
		_Client.SendOffset += _Bytes;
		if (_Client.SendOffset < _Client.SendBuffer.size())
		{
			Write(_Client);
			return;
		}
		++m_Metrics[ECounter::TxPackets];
		_Client.SendBuffer.clear();
		_Client.SendOffset = 0;
		if (_Client.State == EState::Probe)
		{
			_Client.ProbeDelivered = true;
			if (_Client.ProbeKind == 5) shutdown(_Client.Socket, SD_SEND);
			Read(_Client);
		}
		// A login response may arrive before its send completion. Nickname send is scheduled by Tick.
	}
	else
	{
		m_Metrics[ECounter::RxBytes] += _Bytes;
		_Client.Received += _Bytes;
		if (!ProcessPackets(_Client))
		{
			++m_Metrics[ECounter::ProtocolErrors];
			if (_Client.State == EState::Probe) ++m_Metrics[ECounter::ProbeUnexpected];
			Close(_Client, EClose::Failure);
		}
		else if (_Client.State != EState::Closed) Read(_Client);
	}
}

bool CLoadClient::ProcessPackets(SClient& _Client)
{
	size_t Offset = 0;
	while (_Client.Received - Offset >= sizeof(PacketHeader))
	{
		const PacketHeader Header = Decode<PacketHeader>(_Client.ReceiveBuffer.data() + Offset);
		if (Header.packetSize < sizeof(PacketHeader) || Header.packetSize > sizeof(SC_CHAT_BROADCAST)) return false;
		if (_Client.Received - Offset < Header.packetSize) break;
		++m_Metrics[ECounter::RxPackets];
		if (!HandlePacket(_Client, _Client.ReceiveBuffer.data() + Offset, Header)) return false;
		if (_Client.State == EState::Closed) return true;
		Offset += Header.packetSize;
	}
	_Client.Received -= Offset;
	memmove(_Client.ReceiveBuffer.data(), _Client.ReceiveBuffer.data() + Offset, _Client.Received);
	return true;
}

void CLoadClient::StartLogin(SClient& _Client)
{
	CS_LOGIN_REQ Packet;
	const SAccount& Account = m_Config.Accounts[_Client.Index];
	memcpy(Packet.id, Account.ID.data(), Account.ID.size());
	memcpy(Packet.pw, Account.Password.data(), Account.Password.size());
	_Client.State = EState::Login;
	_Client.RequestStart = TClock::now();
	_Client.Deadline = _Client.RequestStart + milliseconds(m_Config.TimeoutMs);
	++m_Metrics[ECounter::LoginSent];
	Send(_Client, MakePacketBuffer(Packet));
}

void CLoadClient::MarkCovered(SClient& _Client)
{
	if (!_Client.Covered)
	{
		_Client.Covered = true;
		++m_Metrics.CoveredClients;
	}
}

void CLoadClient::MarkReady(SClient& _Client)
{
	_Client.State = EState::Ready;
	++m_Metrics.Ready;
	m_Metrics.PeakReady = std::max(m_Metrics.PeakReady, m_Metrics.Ready);
	const TTime Now = TClock::now();
	m_Metrics.Latencies[4].Add(Microseconds(_Client.ConnectStart, Now));
	_Client.NextChat = Now + milliseconds((_Client.Index * 997ULL) % m_Config.IntervalMs);
	MarkCovered(_Client);
}

bool CLoadClient::HandlePacket(SClient& _Client, const char* _Data, const PacketHeader& _Header)
{
	const TTime Now = TClock::now();
	if (_Client.State == EState::Probe)
	{
		if (_Client.ProbeKind != 4 || _Header.packetID != PacketID::SC_LOGIN_RES || _Header.packetSize != sizeof(SC_LOGIN_RES)) return false;
		const auto Packet = Decode<SC_LOGIN_RES>(_Data);
		if (Packet.result != ELoginResult::INVALID_CREDENTIAL) return false;
		++m_Metrics[ECounter::ProbeRejected];
		++m_Metrics.ProbePassed[_Client.ProbeKind];
		MarkCovered(_Client);
		Close(_Client, EClose::ProbeDone);
		return true;
	}
	if (_Header.packetID == PacketID::SC_LOGIN_RES && _Header.packetSize == sizeof(SC_LOGIN_RES) && _Client.State == EState::Login)
	{
		const auto Packet = Decode<SC_LOGIN_RES>(_Data);
		const size_t Result = static_cast<size_t>(Packet.result);
		if (Result >= m_Metrics.LoginResults.size() || Packet.needNickname > 1) return false;
		++m_Metrics.LoginResults[Result];
		if (Packet.result != ELoginResult::SUCCESS)
		{
			++m_Metrics[ECounter::LoginFailed];
			_Client.State = EState::Idle;
			Close(_Client, EClose::Failure);
			return true;
		}
		++m_Metrics[ECounter::LoginOK];
		m_Metrics.Latencies[1].Add(Microseconds(_Client.RequestStart, Now));
		if (Packet.needNickname == 0) MarkReady(_Client);
		else
		{
			_Client.State = EState::Nickname;
			_Client.RequestStart = TTime{}; // Tick starts it once the prior write has completed.
			_Client.Deadline = Now + milliseconds(m_Config.TimeoutMs);
		}
		return true;
	}
	if (_Header.packetID == PacketID::SC_CREATE_NICKNAME_RES && _Header.packetSize == sizeof(SC_CREATE_NICKNAME_RES) &&
		_Client.State == EState::Nickname && _Client.RequestStart != TTime{})
	{
		const auto Packet = Decode<SC_CREATE_NICKNAME_RES>(_Data);
		const size_t Result = static_cast<size_t>(Packet.result);
		if (Result >= m_Metrics.NicknameResults.size()) return false;
		++m_Metrics.NicknameResults[Result];
		if (Packet.result != ENicknameResult::SUCCESS)
		{
			++m_Metrics[ECounter::NicknameFailed];
			_Client.State = EState::Idle;
			Close(_Client, EClose::Failure);
			return true;
		}
		++m_Metrics[ECounter::NicknameOK];
		m_Metrics.Latencies[2].Add(Microseconds(_Client.RequestStart, Now));
		MarkReady(_Client);
		return true;
	}
	if (_Header.packetID == PacketID::SC_CHAT_BROADCAST && _Header.packetSize == sizeof(SC_CHAT_BROADCAST))
	{
		const auto Packet = Decode<SC_CHAT_BROADCAST>(_Data);
		const size_t Length = strnlen_s(Packet.message, sizeof(Packet.message));
		if (Length == sizeof(Packet.message)) return false;
		if (_Client.ChatPending && std::string_view(Packet.message, Length) == _Client.ChatToken)
		{
			++m_Metrics[ECounter::ChatOK];
			m_Metrics.Latencies[3].Add(Microseconds(_Client.RequestStart, Now));
			_Client.ChatPending = false;
		}
		return true;
	}
	if (_Header.packetID == PacketID::SC_CHAT_ERROR && _Header.packetSize == sizeof(SC_CHAT_ERROR) && _Client.ChatPending)
	{
		const auto Packet = Decode<SC_CHAT_ERROR>(_Data);
		const size_t Error = static_cast<size_t>(Packet.error);
		if (Error >= m_Metrics.ChatResults.size()) return false;
		++m_Metrics.ChatResults[Error];
		++m_Metrics[ECounter::ChatErrors];
		_Client.ChatPending = false;
		return true;
	}
	return false;
}

void CLoadClient::StartChat(SClient& _Client, TTime _Now)
{
	CS_CHAT_REQ Packet;
	_Client.ChatToken = m_RunToken + std::to_string(_Client.Index) + ':' + std::to_string(++_Client.ChatSequence);
	memcpy(Packet.message, _Client.ChatToken.data(), _Client.ChatToken.size());
	_Client.ChatPending = true;
	_Client.RequestStart = _Now;
	_Client.Deadline = _Now + milliseconds(m_Config.TimeoutMs);
	++m_Metrics[ECounter::ChatSent];
	Send(_Client, MakePacketBuffer(Packet));
}

void CLoadClient::StartProbe(SClient& _Client)
{
	_Client.State = EState::Probe;
	_Client.ProbeKind = static_cast<size_t>(m_ProbeSequence++ % ProbeNames.size());
	_Client.Deadline = TClock::now() + milliseconds(m_Config.TimeoutMs);
	PacketHeader Header;
	Header.packetSize = sizeof(Header);
	Header.packetID = PacketID::CS_LOGIN_REQ;
	PacketBuffer Buffer;
	switch (_Client.ProbeKind)
	{
	case 0:
		Header.packetSize = 3;
		Buffer = MakePacketBuffer(Header);
		break;
	case 1:
		Header.packetID = static_cast<PacketID>(65535);
		Buffer = MakePacketBuffer(Header);
		break;
	case 2:
		Buffer = MakePacketBuffer(Header);
		break;
	case 3:
		Header.packetSize = sizeof(SD_LOGIN_REQ);
		Header.packetID = PacketID::SD_LOGIN_REQ;
		Buffer.resize(sizeof(SD_LOGIN_REQ), 0);
		memcpy(Buffer.data(), &Header, sizeof(Header));
		break;
	case 4:
	{
		CS_LOGIN_REQ Packet;
		memset(Packet.id, 'x', sizeof(Packet.id));
		memset(Packet.pw, 'x', sizeof(Packet.pw));
		Buffer = MakePacketBuffer(Packet);
		break;
	}
	case 5:
		Buffer = MakePacketBuffer(CS_LOGIN_REQ{});
		Buffer.resize(12);
		break;
	}
	++m_Metrics[ECounter::ProbeSent];
	++m_Metrics.ProbeAttempts[_Client.ProbeKind];
	Send(_Client, std::move(Buffer));
}

void CLoadClient::Tick(TTime _Now)
{
	const double Delta = duration<double>(_Now - m_LastTick).count();
	m_Metrics.MaxTickGapMs = std::max(m_Metrics.MaxTickGapMs, Delta * 1000);
	m_LastTick = _Now;
	// Token caps prevent an overloaded load generator from replaying a large delayed burst.
	m_ConnectTokens = std::min(m_ConnectTokens + Delta * m_Config.ConnectRate, std::max(1.0, m_Config.ConnectRate / 10.0));
	const bool Churn = m_Config.Scenario == "churn" || m_Config.Scenario == "mixed";
	if (Churn) m_ChurnTokens = std::min(m_ChurnTokens + Delta * m_Config.ChurnRate, std::max(1.0, m_Config.ChurnRate / 10.0));
	const bool Force = (m_Config.Scenario == "reconnect" || m_Config.Scenario == "mixed") && _Now >= m_NextFault;
	if (Force) m_NextFault = _Now + milliseconds(m_Config.FaultEveryMs);
	uint64_t ForceRemaining = Force ? std::max<uint64_t>(1, m_Metrics.Ready * m_Config.DisconnectPercent / 100) : 0;
	// Rotate through the population for each fault wave / churn batch.
	const size_t FaultStart = m_FaultCursor;
	for (size_t Offset = 0; Offset < m_Clients.size() && (ForceRemaining > 0 || (Churn && m_ChurnTokens >= 1)); ++Offset)
	{
		SClient& Client = *m_Clients[(FaultStart + Offset) % m_Clients.size()];
		if (Client.State != EState::Ready) continue;
		if (ForceRemaining > 0)
		{
			++m_Metrics[ECounter::ForcedDisconnects];
			Close(Client, EClose::Forced);
			--ForceRemaining;
		}
		else
		{
			++m_Metrics[ECounter::ChurnDisconnects];
			Close(Client, EClose::Churn);
			m_ChurnTokens -= 1;
		}
		m_FaultCursor = (Client.Index + 1) % m_Clients.size();
	}
	const size_t ConnectStart = m_ConnectCursor;
	for (size_t Offset = 0; Offset < m_Clients.size(); ++Offset)
	{
		SClient& Client = *m_Clients[(ConnectStart + Offset) % m_Clients.size()];
		if (Client.State == EState::Closed)
		{
			if (Client.Pending == 0 && _Now >= Client.NextConnect && (m_Config.ConnectRate == 0 || m_ConnectTokens >= 1))
			{
				if (m_Config.ConnectRate != 0) m_ConnectTokens -= 1;
				Connect(Client, TClock::now());
				m_ConnectCursor = (Client.Index + 1) % m_Clients.size();
			}
			continue;
		}
		const bool Waiting = Client.State == EState::Connecting || Client.State == EState::Login ||
			Client.State == EState::Nickname || Client.State == EState::Probe || Client.ChatPending;
		if (Waiting && _Now >= Client.Deadline)
		{
			if (Client.State == EState::Connecting) ++m_Metrics[ECounter::ConnectTimeouts];
			else if (Client.State == EState::Probe) ++m_Metrics[ECounter::ProbeTimeouts];
			else ++m_Metrics[ECounter::RequestTimeouts];
			Close(Client, EClose::Failure);
			continue;
		}
		if (Client.State == EState::Nickname && Client.RequestStart == TTime{} && Client.SendBuffer.empty())
		{
			CS_CREATE_NICKNAME_REQ Packet;
			const auto& Nickname = m_Config.Accounts[Client.Index].Nickname;
			memcpy(Packet.nickname, Nickname.data(), Nickname.size());
			Client.RequestStart = _Now;
			Client.Deadline = _Now + milliseconds(m_Config.TimeoutMs);
			++m_Metrics[ECounter::NicknameSent];
			Send(Client, MakePacketBuffer(Packet));
		}
		const uint64_t ActiveCount = (static_cast<uint64_t>(m_Config.Clients) * m_Config.ActivePercent + 99) / 100;
		// Spread the active subset over all slots; avoid overlap with mixed probes at index % 20 == 0.
		const bool Active = ((Client.Index + 1) * ActiveCount / m_Config.Clients) != (Client.Index * ActiveCount / m_Config.Clients);
		if (Client.State == EState::Ready && Active && _Now >= Client.NextChat)
		{
			Client.NextChat = _Now + milliseconds(m_Config.IntervalMs);
			if (Client.ChatPending || !Client.SendBuffer.empty()) ++m_Metrics[ECounter::ScheduleSkipped];
			else StartChat(Client, _Now);
		}
	}
}

bool CLoadClient::Succeeded() const
{
	if (m_Metrics.CoveredClients != m_Config.Clients) return false;
	for (const auto Counter : { ECounter::ConnectFailures, ECounter::ConnectTimeouts, ECounter::UnexpectedDisconnects,
		ECounter::LoginFailed, ECounter::NicknameFailed, ECounter::ChatErrors, ECounter::RequestTimeouts,
		ECounter::ProtocolErrors, ECounter::IOErrors, ECounter::ProbeUnexpected, ECounter::ProbeTimeouts })
	{
		if (m_Metrics[Counter] != 0) return false;
	}
	return true;
}

void CLoadClient::Report(TTime _Now, bool _Final)
{
	const double Elapsed = duration<double>(_Now - m_Start).count();
	const double Period = std::max(0.000001, _Final ? Elapsed : duration<double>(_Now - m_LastReport).count());
	const auto Rate = [&](ECounter Counter)
	{
		return static_cast<double>(m_Metrics[Counter] - (_Final ? 0 : m_Previous[Counter])) / Period;
	};
	std::cout << std::fixed << std::setprecision(2) << (_Final ? "FINAL " : "STAT ") << "t=" << Elapsed
		<< " connected=" << m_Metrics.Connected << " ready=" << m_Metrics.Ready << " peak=" << m_Metrics.PeakReady
		<< " login_tps=" << Rate(ECounter::LoginOK) << " chat_tps=" << Rate(ECounter::ChatOK)
		<< " tx_pkt/s=" << Rate(ECounter::TxPackets) << " rx_pkt/s=" << Rate(ECounter::RxPackets)
		<< " tx_MiB/s=" << Rate(ECounter::TxBytes) / (1024 * 1024) << " rx_MiB/s=" << Rate(ECounter::RxBytes) / (1024 * 1024)
		<< " chat_ms(avg/p95/p99)=" << m_Metrics.Latencies[3].AverageMs() << '/'
		<< m_Metrics.Latencies[3].PercentileMs(0.95) << '/' << m_Metrics.Latencies[3].PercentileMs(0.99)
		<< " timeouts=" << m_Metrics[ECounter::RequestTimeouts] + m_Metrics[ECounter::ConnectTimeouts] + m_Metrics[ECounter::ProbeTimeouts]
		<< " login_fail=" << m_Metrics[ECounter::LoginFailed] << " chat_errors=" << m_Metrics[ECounter::ChatErrors] << '\n';
	if (m_Csv.is_open())
	{
		m_Csv << std::fixed << std::setprecision(6) << (_Final ? "final" : "sample") << ',' << Elapsed << ','
			<< m_Metrics.Connected << ',' << m_Metrics.Ready << ',' << m_Metrics.PeakConnected << ',' << m_Metrics.PeakReady << ','
			<< m_Metrics.CoveredClients << ',' << m_Metrics.MaxTickGapMs;
		for (const auto Counter : { ECounter::Connects, ECounter::LoginOK, ECounter::ChatOK, ECounter::TxPackets,
			ECounter::RxPackets, ECounter::TxBytes, ECounter::RxBytes }) m_Csv << ',' << Rate(Counter);
		for (const auto Value : m_Metrics.Counters) m_Csv << ',' << Value;
		for (const auto& Latency : m_Metrics.Latencies) Latency.WriteCsv(m_Csv);
		for (const auto Value : m_Metrics.LoginResults) m_Csv << ',' << Value;
		for (const auto Value : m_Metrics.NicknameResults) m_Csv << ',' << Value;
		for (const auto Value : m_Metrics.ChatResults) m_Csv << ',' << Value;
		for (size_t Index = 0; Index < ProbeNames.size(); ++Index)
			m_Csv << ',' << m_Metrics.ProbeAttempts[Index] << ',' << m_Metrics.ProbePassed[Index];
		m_Csv << '\n';
		m_Csv.flush();
		if (!m_Csv) throw std::runtime_error("CSV write failed");
	}
	if (_Final)
	{
		for (size_t Index = 0; Index < CounterNames.size(); ++Index)
			std::cout << CounterNames[Index] << '=' << m_Metrics.Counters[Index] << (Index % 5 == 4 ? '\n' : ' ');
		std::cout << '\n';
		for (size_t Index = 0; Index < LatencyNames.size(); ++Index)
		{
			const auto& Latency = m_Metrics.Latencies[Index];
			std::cout << LatencyNames[Index] << " samples=" << Latency.Count() << " avg_ms=" << Latency.AverageMs()
				<< " p95_ms=" << Latency.PercentileMs(0.95) << " p99_ms=" << Latency.PercentileMs(0.99) << '\n';
		}
		for (size_t Index = 0; Index < ProbeNames.size(); ++Index)
			if (m_Metrics.ProbeAttempts[Index] != 0)
				std::cout << "probe=" << ProbeNames[Index] << " sent=" << m_Metrics.ProbeAttempts[Index] << " passed=" << m_Metrics.ProbePassed[Index] << '\n';
		for (const auto& [Error, Count] : m_IOErrors) std::cout << "socket_error=" << Error << " count=" << Count << '\n';
	}
	m_Previous.Counters = m_Metrics.Counters;
	m_LastReport = _Now;
}
