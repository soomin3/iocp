#pragma once
#include "User.h"

#define USER_SIZE 100

using namespace std;

class CUserManager
{
public:
	CUserManager();
	~CUserManager();

	void Init();
	//bool PostAccept(SOCKET _ListenSocket);

	shared_ptr<CUser> PopUser();
	void PushUser(int32_t _UserID);
private:
	CRITICAL_SECTION m_UserPoolLock;
	unordered_map<int32_t, shared_ptr<CUser>> m_ActiveUserMap;
	queue<shared_ptr<CUser>> m_UserPool;
};
