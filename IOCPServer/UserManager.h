#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include "User.h"

using namespace std;

class CSession;

struct SActiveUserContext
{
	shared_ptr<CUser> User;
	CSession* Session = nullptr;
	uint64_t Generation = 0;
};

class CUserManager
{
public:
	CUserManager();
	~CUserManager();

	shared_ptr<CUser> TryAddUser(int64_t _AccountID, CSession& _Session, uint64_t _Generation);
	bool FindUser(int64_t _AccountID, SActiveUserContext& _Context);
	vector<SActiveUserContext> GetActiveUsers();
	bool RemoveUser(int64_t _AccountID, const shared_ptr<CUser>& _ExpectedUser);
	size_t GetUserCount();

private:
	CRITICAL_SECTION m_UserLock;
	unordered_map<int64_t, SActiveUserContext> m_ActiveUserMap;
};
