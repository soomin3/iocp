#pragma once
#include "Packet.h"

class CSession;
struct PacketHeader;

class CPacketManager
{
public:
	CPacketManager();
	~CPacketManager() = default;

	bool ProcessPacket(CSession& _Session, const char* _Buffer, PacketHeader _Header);

private:
};