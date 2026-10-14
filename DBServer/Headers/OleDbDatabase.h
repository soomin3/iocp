#pragma once

#include <WinSock2.h>
#include <Windows.h>
#include <atlbase.h>
#include <atldbcli.h>

#include "Packet.h"

class COleDbDatabase
{
public:
	bool Open();
	PacketBuffer Handle(const PacketBuffer& _Request);

private:
	PacketBuffer Login(const SD_LOGIN_REQ& _Request);
	PacketBuffer CreateNickname(const SD_CREATE_NICKNAME_REQ& _Request);

	ATL::CDataSource m_DataSource;
	ATL::CSession m_Session;
};
