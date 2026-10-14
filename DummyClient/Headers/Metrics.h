#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <string_view>

enum class ECounter : size_t
{
	Attempts, Connects, ConnectFailures, ConnectTimeouts, Reconnects,
	Disconnects, UnexpectedDisconnects, ForcedDisconnects, ChurnDisconnects,
	LoginSent, LoginOK, LoginFailed, NicknameSent, NicknameOK, NicknameFailed,
	ChatSent, ChatOK, ChatErrors, ScheduleSkipped, RequestTimeouts, AbandonedRequests,
	ShutdownCanceled, ProtocolErrors, IOErrors, TxPackets, RxPackets, TxBytes, RxBytes,
	ProbeSent, ProbeClosed, ProbeRejected, ProbeUnexpected, ProbeTimeouts, Count
};

inline constexpr std::array<std::string_view, static_cast<size_t>(ECounter::Count)> CounterNames{
	"attempts", "connects", "connect_failures", "connect_timeouts", "reconnects",
	"disconnects", "unexpected_disconnects", "forced_disconnects", "churn_disconnects",
	"login_sent", "login_ok", "login_failed", "nickname_sent", "nickname_ok", "nickname_failed",
	"chat_sent", "chat_ok", "chat_errors", "schedule_skipped", "request_timeouts", "abandoned_requests",
	"shutdown_canceled", "protocol_errors", "io_errors", "tx_packets", "rx_packets", "tx_bytes", "rx_bytes",
	"probe_sent", "probe_closed", "probe_rejected", "probe_unexpected", "probe_timeouts"
};

// 32 subdivisions per power of two: bounded memory, percentile error <=3.125%.
// Exact mean, nearest-rank percentiles reported at the upper edge of a bucket.
class CLatency
{
public:
	void Add(uint64_t _Microseconds)
	{
		++m_Count;
		m_Sum += static_cast<long double>(_Microseconds);
		m_Max = std::max(m_Max, _Microseconds);
		size_t Index = static_cast<size_t>(_Microseconds);
		if (_Microseconds >= 32)
		{
			const unsigned Exponent = static_cast<unsigned>(std::bit_width(_Microseconds) - 1);
			Index = 32 + (Exponent - 5) * 32 + static_cast<size_t>((_Microseconds >> (Exponent - 5)) - 32);
		}
		++m_Buckets[Index];
	}
	uint64_t Count() const { return m_Count; }
	double AverageMs() const { return m_Count == 0 ? 0 : static_cast<double>(m_Sum / m_Count / 1000); }
	double PercentileMs(double _Percentile) const
	{
		if (m_Count == 0) return 0;
		const uint64_t Rank = static_cast<uint64_t>(std::ceil(static_cast<double>(m_Count) * _Percentile));
		uint64_t Seen = 0;
		for (size_t Index = 0; Index < m_Buckets.size(); ++Index)
		{
			Seen += m_Buckets[Index];
			if (Seen < Rank) continue;
			uint64_t Upper = static_cast<uint64_t>(Index);
			if (Index >= 32)
			{
				const size_t Shift = (Index - 32) / 32;
				Upper = (static_cast<uint64_t>(33 + (Index - 32) % 32) << Shift) - 1ULL;
			}
			return static_cast<double>(std::min(Upper, m_Max)) / 1000.0;
		}
		return static_cast<double>(m_Max) / 1000.0;
	}
	void WriteCsv(std::ostream& _Output) const
	{
		_Output << ',' << Count() << ',' << AverageMs() << ',' << PercentileMs(0.95) << ',' << PercentileMs(0.99);
	}
private:
	std::array<uint64_t, 1920> m_Buckets{};
	uint64_t m_Count = 0;
	uint64_t m_Max = 0;
	long double m_Sum = 0;
};

struct SMetrics
{
	std::array<uint64_t, static_cast<size_t>(ECounter::Count)> Counters{};
	uint64_t& operator[](ECounter _Counter) { return Counters[static_cast<size_t>(_Counter)]; }
	uint64_t operator[](ECounter _Counter) const { return Counters[static_cast<size_t>(_Counter)]; }
	// Connect, successful login, successful nickname, successful chat, connect-to-ready.
	std::array<CLatency, 5> Latencies;
	std::array<uint64_t, 5> LoginResults{};
	std::array<uint64_t, 7> NicknameResults{};
	std::array<uint64_t, 5> ChatResults{};
	std::array<uint64_t, 6> ProbeAttempts{};
	std::array<uint64_t, 6> ProbePassed{};
	uint64_t Connected = 0;
	uint64_t Ready = 0;
	uint64_t PeakConnected = 0;
	uint64_t PeakReady = 0;
	uint64_t CoveredClients = 0;
	double MaxTickGapMs = 0;
};

inline constexpr std::array<std::string_view, 5> LatencyNames{ "connect", "login", "nickname", "chat", "ready" };
inline constexpr std::array<std::string_view, 6> ProbeNames{
	"short-header-size", "unknown-id", "wrong-login-size", "forged-internal", "unterminated-login", "truncated-login"
};
