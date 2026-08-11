#include "pch.h"
#include "PacketManager.h"
#include "Session.h"

CPacketManager::CPacketManager()
{
}

bool CPacketManager::ProcessPacket(CSession& _Session, const char* _Buffer, PacketHeader _Header)
{
	switch (_Header.packetID)
	{
	case PacketID::CS_LOGIN_REQ:
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
	}
	break;
	default:
		return false;
	}
	return true;
}
