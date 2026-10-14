#pragma once
#include <cassert>
#include <coroutine>
#include <exception>
#include <utility>

// Session owns the frame until every I/O completion and resume call has returned.
class CReceiveTask
{
public:
	struct promise_type
	{
		CReceiveTask get_return_object() noexcept
		{
			return CReceiveTask(std::coroutine_handle<promise_type>::from_promise(*this));
		}
		std::suspend_always initial_suspend() const noexcept { return {}; }
		std::suspend_always final_suspend() const noexcept { return {}; }
		void return_void() const noexcept {}
		void unhandled_exception() const noexcept { std::terminate(); }
	};

	CReceiveTask() = default;
	CReceiveTask(const CReceiveTask&) = delete;
	CReceiveTask& operator=(const CReceiveTask&) = delete;
	CReceiveTask(CReceiveTask&& _Other) noexcept
		: m_Handle(std::exchange(_Other.m_Handle, {})) {}
	CReceiveTask& operator=(CReceiveTask&& _Other) noexcept
	{
		if (this != &_Other)
		{
			Reset();
			m_Handle = std::exchange(_Other.m_Handle, {});
		}
		return *this;
	}
	~CReceiveTask() { Reset(); }

	// Only inspect completion after the session's pending I/O count reaches zero.
	bool IsDone() const noexcept { return !m_Handle || m_Handle.done(); }
	void Resume() const
	{
		assert(m_Handle);
		// A subsequent completion may resume again before this call returns.
		// Do not inspect the promise/frame after publishing a receive operation.
		m_Handle.resume();
	}
	void Reset() noexcept
	{
		if (m_Handle)
		{
			m_Handle.destroy();
			m_Handle = {};
		}
	}

private:
	explicit CReceiveTask(std::coroutine_handle<promise_type> _Handle) noexcept : m_Handle(_Handle) {}
	std::coroutine_handle<promise_type> m_Handle{};
};
