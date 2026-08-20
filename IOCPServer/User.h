#pragma once

using namespace std;

class CUser : public enable_shared_from_this<CUser>
{
public:
	CUser() : m_UserID(0) {}
	//CUser(int32_t _UserID);
	//~CUser() = default;

	//void SetUserID(int32_t _UserID) { m_UserID = _UserID; }
	int32_t GetUserID() const { return m_UserID; }
	//bool PostAccept(SOCKET _ListenSocket) { return m_Session->PostAccept(_ListenSocket, m_UserID); }
	//bool PostRead() { return m_Session->PostRead(m_UserID); }

	//void PrintReadBuffer();
private:
	int32_t m_UserID;
};