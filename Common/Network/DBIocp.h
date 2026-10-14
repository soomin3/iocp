#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include <Windows.h>
#include <cassert>
#include <coroutine>
#include <exception>
#include <limits>
#include <utility>
#include "DBTransport.h"

namespace DBIocp
{
	// One worker owns each context, its tasks, and socket operations. Only Wake may
	// be called from another thread. The DB protocol has one outstanding operation.
	class CTask
	{
	public:
		struct promise_type
		{
			bool Result = false;
			std::coroutine_handle<> Continuation{};
			CTask get_return_object() noexcept
			{
				return CTask(std::coroutine_handle<promise_type>::from_promise(*this));
			}
			std::suspend_always initial_suspend() const noexcept { return {}; }
			struct SFinalAwaiter
			{
				bool await_ready() const noexcept { return false; }
				std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> _Handle) const noexcept
				{
					const auto Next = _Handle.promise().Continuation;
					return Next ? Next : std::noop_coroutine();
				}
				void await_resume() const noexcept {}
			};
			SFinalAwaiter final_suspend() const noexcept { return {}; }
			void return_value(bool _Result) noexcept { Result = _Result; }
			void unhandled_exception() const noexcept { std::terminate(); }
		};

		CTask() = default;
		CTask(const CTask&) = delete;
		CTask& operator=(const CTask&) = delete;
		CTask(CTask&& _Other) noexcept : m_Handle(std::exchange(_Other.m_Handle, {})) {}
		CTask& operator=(CTask&& _Other) noexcept
		{
			if (this != &_Other)
			{
				Reset();
				m_Handle = std::exchange(_Other.m_Handle, {});
			}
			return *this;
		}
		~CTask() { Reset(); }

		bool IsDone() const noexcept { return !m_Handle || m_Handle.done(); }
		void Resume() const { assert(m_Handle && !m_Handle.done()); m_Handle.resume(); }
		// A started task may only be reset after completion and I/O cancellation drain.
		void Reset() noexcept
		{
			if (m_Handle) { m_Handle.destroy(); m_Handle = {}; }
		}
		struct SAwaiter
		{
			std::coroutine_handle<promise_type> Handle;
			bool await_ready() const noexcept { return false; }
			std::coroutine_handle<> await_suspend(std::coroutine_handle<> _Continuation) const noexcept
			{
				Handle.promise().Continuation = _Continuation;
				return Handle;
			}
			bool await_resume() const noexcept { return Handle.promise().Result; }
		};
		SAwaiter operator co_await() && noexcept { return { m_Handle }; }

	private:
		explicit CTask(std::coroutine_handle<promise_type> _Handle) noexcept : m_Handle(_Handle) {}
		std::coroutine_handle<promise_type> m_Handle{};
	};

	struct SResult
	{
		DWORD Bytes = 0;
		DWORD Error = ERROR_SUCCESS;
	};

	class COperation;
	class CContext
	{
	public:
		CContext() = default;
		CContext(const CContext&) = delete;
		CContext& operator=(const CContext&) = delete;
		~CContext() { Close(); }
		bool Open()
		{
			if (m_Port) return false;
			m_Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
			return m_Port != nullptr;
		}
		void Close()
		{
			assert(!m_Pending);
			if (m_Port) { CloseHandle(m_Port); m_Port = nullptr; }
		}
		bool Associate(SOCKET _Socket) const
		{
			return m_Port && CreateIoCompletionPort(reinterpret_cast<HANDLE>(_Socket), m_Port, 0, 0) == m_Port;
		}
		// Callers must stop producers before closing the context.
		bool Wake() const { return m_Port && PostQueuedCompletionStatus(m_Port, 0, 0, nullptr); }
		void RunOne(DWORD _ExternalWait = INFINITE);
		void CancelAll();
		size_t PendingCount() const noexcept { return m_Pending ? 1 : 0; }

	private:
		friend class COperation;
		void Cancel(DWORD _Error);
		HANDLE m_Port = nullptr;
		COperation* m_Pending = nullptr;
	};

	class COperation
	{
	public:
		enum class EType { Receive, Send, Connect, Accept };
		COperation(CContext& _Context, SOCKET _Socket, EType _Type, ULONGLONG _Deadline)
			: m_Context(_Context), m_Socket(_Socket), m_Type(_Type), m_Deadline(_Deadline) {}
		COperation(CContext& _Context, SOCKET _Socket, EType _Type, ULONGLONG _Deadline, char* _Buffer, size_t _Size)
			: COperation(_Context, _Socket, _Type, _Deadline)
		{
			assert(_Size <= (std::numeric_limits<ULONG>::max)());
			Buffer = { static_cast<ULONG>(_Size), _Buffer };
		}
		COperation(const COperation&) = delete;
		COperation& operator=(const COperation&) = delete;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<> _Continuation);
		SResult await_resume() const noexcept { return m_Result; }

		WSABUF Buffer{};
		sockaddr_in Address{};
		SOCKET AcceptedSocket = INVALID_SOCKET;
		LPFN_CONNECTEX ConnectEx = nullptr;
		LPFN_ACCEPTEX AcceptEx = nullptr;

	private:
		friend class CContext;
		CContext& m_Context;
		SOCKET m_Socket;
		EType m_Type;
		ULONGLONG m_Deadline;
		OVERLAPPED m_Overlapped{};
		std::coroutine_handle<> m_Continuation{};
		SResult m_Result;
		DWORD m_CancelError = ERROR_SUCCESS;
		char m_AcceptAddresses[(sizeof(sockaddr_in) + 16) * 2]{};
	};

	inline bool COperation::await_suspend(std::coroutine_handle<> _Continuation)
	{
		assert(!m_Context.m_Pending);
		if (m_Deadline && GetTickCount64() >= m_Deadline)
		{
			m_Result.Error = WSAETIMEDOUT;
			return false;
		}
		m_Continuation = _Continuation;
		m_Context.m_Pending = this;
		DWORD Flags = 0;
		DWORD Bytes = 0;
		bool Started = false;
		switch (m_Type)
		{
		case EType::Receive:
			Started = WSARecv(m_Socket, &Buffer, 1, nullptr, &Flags, &m_Overlapped, nullptr) == 0;
			break;
		case EType::Send:
			Started = WSASend(m_Socket, &Buffer, 1, nullptr, 0, &m_Overlapped, nullptr) == 0;
			break;
		case EType::Connect:
			Started = ConnectEx(m_Socket, reinterpret_cast<sockaddr*>(&Address), sizeof(Address),
				nullptr, 0, nullptr, &m_Overlapped) != FALSE;
			break;
		case EType::Accept:
			Started = AcceptEx(m_Socket, AcceptedSocket, m_AcceptAddresses, 0,
				sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &Bytes, &m_Overlapped) != FALSE;
			break;
		}
		const DWORD Error = Started ? ERROR_SUCCESS : static_cast<DWORD>(WSAGetLastError());
		// Immediate success still produces an IOCP completion. Only immediate failure
		// bypasses suspension; the coroutine owns OVERLAPPED until completion is drained.
		if (Started || Error == WSA_IO_PENDING) return true;
		m_Result.Error = Error;
		m_Context.m_Pending = nullptr;
		return false;
	}

	inline void CContext::Cancel(DWORD _Error)
	{
		if (m_Pending && m_Pending->m_CancelError == ERROR_SUCCESS)
		{
			m_Pending->m_CancelError = _Error;
			CancelIoEx(reinterpret_cast<HANDLE>(m_Pending->m_Socket), &m_Pending->m_Overlapped);
		}
	}

	inline void CContext::CancelAll() { Cancel(ERROR_OPERATION_ABORTED); }

	inline void CContext::RunOne(DWORD _ExternalWait)
	{
		assert(m_Port);
		DWORD Wait = _ExternalWait;
		if (m_Pending && m_Pending->m_Deadline && m_Pending->m_CancelError == ERROR_SUCCESS)
		{
			const ULONGLONG Now = GetTickCount64();
			const ULONGLONG Remaining = m_Pending->m_Deadline > Now ? m_Pending->m_Deadline - Now : 0;
			if (Remaining == 0) Cancel(WSAETIMEDOUT);
			else if (Remaining < Wait) Wait = static_cast<DWORD>(Remaining);
		}
		DWORD Bytes = 0;
		ULONG_PTR Key = 0;
		OVERLAPPED* Overlapped = nullptr;
		const BOOL Success = GetQueuedCompletionStatus(m_Port, &Bytes, &Key, &Overlapped, Wait);
		const DWORD Error = Success ? ERROR_SUCCESS : GetLastError();
		if (!Overlapped)
		{
			if (m_Pending && m_Pending->m_Deadline &&
				GetTickCount64() >= m_Pending->m_Deadline)
			{
				Cancel(WSAETIMEDOUT);
			}
			return;
		}
		COperation* Operation = m_Pending;
		assert(Operation && Overlapped == &Operation->m_Overlapped);
		if (Operation->m_CancelError == ERROR_SUCCESS && Operation->m_Deadline &&
			GetTickCount64() >= Operation->m_Deadline) Operation->m_CancelError = WSAETIMEDOUT;
		Operation->m_Result.Bytes = Bytes;
		Operation->m_Result.Error = Operation->m_CancelError != ERROR_SUCCESS ? Operation->m_CancelError : Error;
		const auto Continuation = Operation->m_Continuation;
		m_Pending = nullptr;
		Continuation.resume();
		// Resume can finish and destroy the operation's frame; do not access it here.
	}

	inline COperation Receive(CContext& _Context, SOCKET _Socket, char* _Buffer, size_t _Size, ULONGLONG _Deadline = 0)
	{
		return COperation(_Context, _Socket, COperation::EType::Receive, _Deadline, _Buffer, _Size);
	}

	inline COperation Send(CContext& _Context, SOCKET _Socket, const char* _Buffer, size_t _Size, ULONGLONG _Deadline = 0)
	{
		return COperation(_Context, _Socket, COperation::EType::Send, _Deadline, const_cast<char*>(_Buffer), _Size);
	}

	inline CTask Connect(CContext& _Context, SOCKET _Socket, sockaddr_in _Address, ULONGLONG _Deadline)
	{
		if (!_Context.Associate(_Socket)) co_return false;
		sockaddr_in LocalAddress{};
		LocalAddress.sin_family = AF_INET;
		if (::bind(_Socket, reinterpret_cast<sockaddr*>(&LocalAddress), sizeof(LocalAddress)) != 0) co_return false;
		COperation Operation(_Context, _Socket, COperation::EType::Connect, _Deadline);
		GUID ConnectID = WSAID_CONNECTEX;
		DWORD Bytes = 0;
		if (WSAIoctl(_Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &ConnectID, sizeof(ConnectID),
			&Operation.ConnectEx, sizeof(Operation.ConnectEx), &Bytes, nullptr, nullptr) != 0) co_return false;
		Operation.Address = _Address;
		const SResult Result = co_await Operation;
		co_return Result.Error == ERROR_SUCCESS &&
			setsockopt(_Socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == 0;
	}

	inline CTask Accept(CContext& _Context, SOCKET _Listener, SOCKET _Accepted)
	{
		COperation Operation(_Context, _Listener, COperation::EType::Accept, 0);
		GUID AcceptID = WSAID_ACCEPTEX;
		DWORD Bytes = 0;
		if (WSAIoctl(_Listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &AcceptID, sizeof(AcceptID),
			&Operation.AcceptEx, sizeof(Operation.AcceptEx), &Bytes, nullptr, nullptr) != 0) co_return false;
		Operation.AcceptedSocket = _Accepted;
		const SResult Result = co_await Operation;
		co_return Result.Error == ERROR_SUCCESS &&
			setsockopt(_Accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&_Listener), sizeof(_Listener)) == 0 &&
			_Context.Associate(_Accepted);
	}

	inline CTask SendPacket(CContext& _Context, SOCKET _Socket, const PacketBuffer& _Packet)
	{
		const ULONGLONG Deadline = GetTickCount64() + DBTransport::TIMEOUT_MS;
		size_t Offset = 0;
		while (Offset < _Packet.size())
		{
			const SResult Result = co_await Send(_Context, _Socket, _Packet.data() + Offset, _Packet.size() - Offset, Deadline);
			if (Result.Error != ERROR_SUCCESS || Result.Bytes == 0) co_return false;
			Offset += Result.Bytes;
		}
		co_return true;
	}

	inline CTask ReceivePacket(CContext& _Context, SOCKET _Socket, PacketBuffer& _Packet, bool _Request, bool _AllowIdle = false)
	{
		ULONGLONG Deadline = _AllowIdle ? 0 : GetTickCount64() + DBTransport::TIMEOUT_MS;
		PacketHeader Header;
		size_t Offset = 0;
		while (Offset < sizeof(Header))
		{
			const size_t Size = _AllowIdle && Offset == 0 ? 1 : sizeof(Header) - Offset;
			const SResult Result = co_await Receive(_Context, _Socket, reinterpret_cast<char*>(&Header) + Offset, Size, Deadline);
			if (Result.Error != ERROR_SUCCESS || Result.Bytes == 0) co_return false;
			Offset += Result.Bytes;
			if (!Deadline) Deadline = GetTickCount64() + DBTransport::TIMEOUT_MS;
		}
		const size_t Size = DBTransport::PacketSize(Header.packetID, _Request);
		if (Size == 0 || Header.packetSize != Size) co_return false;
		_Packet.resize(Size);
		memcpy(_Packet.data(), &Header, sizeof(Header));
		while (Offset < Size)
		{
			const SResult Result = co_await Receive(_Context, _Socket, _Packet.data() + Offset, Size - Offset, Deadline);
			if (Result.Error != ERROR_SUCCESS || Result.Bytes == 0) co_return false;
			Offset += Result.Bytes;
		}
		co_return true;
	}
}
