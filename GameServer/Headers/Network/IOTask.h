#pragma once
#include <cassert>
#include <coroutine>
#include <exception>
#include <utility>

// Session owns the frame until every I/O completion and resume call has returned.
class CIOTask
{
public:
	struct promise_type
	{
		CIOTask get_return_object() noexcept
		{
			return CIOTask(std::coroutine_handle<promise_type>::from_promise(*this));
		}
		std::suspend_always initial_suspend() const noexcept { return {}; }
		std::suspend_always final_suspend() const noexcept { return {}; }
		void return_void() const noexcept {}
		void unhandled_exception() const noexcept { std::terminate(); }
	};

	CIOTask() = default;
	CIOTask(const CIOTask&) = delete;
	CIOTask& operator=(const CIOTask&) = delete;
	CIOTask(CIOTask&& _Other) noexcept
		: m_Handle(std::exchange(_Other.m_Handle, {})) {}
	CIOTask& operator=(CIOTask&& _Other) noexcept
	{
		if (this != &_Other)
		{
			Reset();
			m_Handle = std::exchange(_Other.m_Handle, {});
		}
		return *this;
	}
	~CIOTask() { Reset(); }

	// Only inspect completion after the session's pending I/O count reaches zero.
	bool IsDone() const noexcept { return !m_Handle || m_Handle.done(); }
	void Resume() const
	{
		assert(m_Handle);
		// A subsequent completion may resume again before this call returns.
		// Do not inspect the promise/frame after publishing asynchronous work.
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
	explicit CIOTask(std::coroutine_handle<promise_type> _Handle) noexcept : m_Handle(_Handle) {}
	std::coroutine_handle<promise_type> m_Handle{};
};
