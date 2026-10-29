#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include <cstdio>
#include <string_view>

namespace LifetimeVerification
{
	// Each scenario runs in a disposable child process. Do not unwind frames
	// that can still own kernel I/O when an invariant fails.
	inline void Check(bool Condition, const char* Message)
	{
		if (Condition) return;
		std::fprintf(stderr, "FAIL: %s (Win32=%lu, WSA=%d)\n", Message, GetLastError(), WSAGetLastError());
		std::fflush(stderr);
		ExitProcess(1);
	}

	inline void Log(const char* Message)
	{
		std::printf("%s\n", Message);
		std::fflush(stdout);
	}

	void RunSessionLifetimeCase(std::string_view Id);
	void RunDBLifetimeCase(std::string_view Id);
	void RunGameServerCase(std::string_view Id);
}
