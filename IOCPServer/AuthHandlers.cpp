#include "pch.h"
#include "AuthHandlers.h"

bool Handle_CS_LOGIN_REQ(CSession& _Session, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Header.packetSize != sizeof(CS_LOGIN_REQ))
	{
		return false;
	}

	CS_LOGIN_REQ Packet{};
	memcpy(&Packet, _Buffer, sizeof(Packet));

	SC_LOGIN_RES SendPacket;
	SendPacket.result = true;

	if (!_Session.Send(&SendPacket, sizeof(SendPacket)))
	{
		return false;
	}

	return true;
}
