#include "pch.h"
#include "UserManager.h"

CUserManager::CUserManager()
{
	InitializeCriticalSection(&m_UserLock);
}

CUserManager::~CUserManager()
{
	DeleteCriticalSection(&m_UserLock);
}

shared_ptr<CUser> CUserManager::TryAddUser(int64_t _AccountID, CSession& _Session, uint64_t _Generation)
{
	if (_AccountID <= 0)
	{
		return nullptr;
	}

	CCSLock Lock(&m_UserLock);

	if (m_ActiveUserMap.contains(_AccountID))
	{
		return nullptr;
	}

	shared_ptr<CUser> User = make_shared<CUser>(_AccountID);

	m_ActiveUserMap.emplace(_AccountID, SActiveUserContext{ User, &_Session, _Generation });

	return User;
}

bool CUserManager::FindUser(int64_t _AccountID, SActiveUserContext& _Context)
{
	CCSLock Lock(&m_UserLock);

	const auto UserIt = m_ActiveUserMap.find(_AccountID);
	if (UserIt == m_ActiveUserMap.end())
	{
		return false;
	}

	_Context = UserIt->second;
	return true;
}

vector<SActiveUserContext> CUserManager::GetActiveUsers()
{
	CCSLock Lock(&m_UserLock);

	vector<SActiveUserContext> Users;
	Users.reserve(m_ActiveUserMap.size());

	for (const auto& UserEntry : m_ActiveUserMap)
	{
		Users.push_back(UserEntry.second);
	}

	return Users;
}

bool CUserManager::RemoveUser(int64_t _AccountID, const shared_ptr<CUser>& _ExpectedUser)
{
	if (!_ExpectedUser)
	{
		return false;
	}

	CCSLock Lock(&m_UserLock);

	auto iter = m_ActiveUserMap.find(_AccountID);

	if (iter == m_ActiveUserMap.end() || iter->second.User != _ExpectedUser)
	{
		return false;
	}

	m_ActiveUserMap.erase(iter);

	return true;
}

size_t CUserManager::GetUserCount()
{
	CCSLock Lock(&m_UserLock);
	return m_ActiveUserMap.size();
}
