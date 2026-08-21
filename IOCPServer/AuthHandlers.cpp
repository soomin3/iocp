#include "pch.h"
#include "AuthHandlers.h"
#include "Packet.h"
#include "PacketDispatcher.h"
#include "Session.h"
#include "DBManager.h"

namespace
{
	bool Handle_CS_LOGIN_REQ(CDBManager& _DBManager, CSession& _Session, const char* _Buffer, const PacketHeader& _Header)
	{
		if (_Header.packetSize != sizeof(CS_LOGIN_REQ))
		{
			return false;
		}

		CS_LOGIN_REQ Packet{};
		memcpy(&Packet, _Buffer, sizeof(Packet));

		string ID(Packet.id, strnlen_s(Packet.id, sizeof(Packet.id)));
		string PW(Packet.pw, strnlen_s(Packet.pw, sizeof(Packet.pw)));

		CSession* Session = &_Session;
		const uint64_t Generation = _Session.GetGeneration();

		return _DBManager.AuthenticateAsync(move(ID), move(PW), [Session, Generation](SLoginProcedureResult _Result)
			{
				SC_LOGIN_RES SendPacket;
				SendPacket.result = (_Result.Executed && _Result.ResultCode == 0);
				Session->Send(&SendPacket, sizeof(SendPacket), Generation);
			});
	}
}

bool RegisterAuthHandlers(CPacketDispatcher& _PacketDispatcher, CDBManager& _DBManager)
{
	CDBManager* DBManager = &_DBManager;

	return _PacketDispatcher.Register(PacketID::CS_LOGIN_REQ, [DBManager](CSession& _Session, const char* _Buffer, const PacketHeader& _Header)
		{
			return Handle_CS_LOGIN_REQ(*DBManager, _Session, _Buffer, _Header);
		});
}
