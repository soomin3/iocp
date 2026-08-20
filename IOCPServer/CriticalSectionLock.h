#pragma once

using namespace std;

class CCSLock
{
public:
	CCSLock(LPCRITICAL_SECTION _CriticalSection) : m_CriticalSection(_CriticalSection)
	{
		EnterCriticalSection(m_CriticalSection);
	}

	~CCSLock()
	{
		LeaveCriticalSection(m_CriticalSection);
	}
private:
	CRITICAL_SECTION* m_CriticalSection;
};