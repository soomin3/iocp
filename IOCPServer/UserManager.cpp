#include "UserManager.h"
#include "CriticalSectionLock.h"

CUserManager::CUserManager()
{
	InitializeCriticalSection(&m_UserPoolLock);
}

CUserManager::~CUserManager()
{
	DeleteCriticalSection(&m_UserPoolLock);
}

void CUserManager::Init()
{
	CCSLock lock(&m_UserPoolLock);

	for (int32_t i = 0; i < USER_SIZE; ++i)
	{
		shared_ptr<CUser> user = make_shared<CUser>();
		m_UserPool.emplace(user);
	}
}

//bool CUserManager::PostAccept(SOCKET _ListenSocket)
//{
//	CCSLock lock(&m_UserPoolLock);
//
//	for (int32_t i = 0; i < USER_SIZE; ++i)
//	{
//		shared_ptr<CUser> user = make_shared<CUser>(i);
//		user->PostAccept(_ListenSocket);
//		m_UserPool.emplace(user);
//	}
//	return false;
//}

shared_ptr<CUser> CUserManager::PopUser()
{
	CCSLock lock(&m_UserPoolLock);

	if (!m_UserPool.empty())
	{
		shared_ptr<CUser> user = m_UserPool.front();
		m_UserPool.pop();
		m_ActiveUserMap.emplace(user->GetUserID(), user);

		return user;
	}

	return nullptr;
}

void CUserManager::PushUser(int32_t _UserID)
{
	CCSLock lock(&m_UserPoolLock);

	auto it = m_ActiveUserMap.find(_UserID);
	if (it != m_ActiveUserMap.end())
	{
		
		m_UserPool.emplace(it->second);
		m_ActiveUserMap.erase(it);
	}	
}