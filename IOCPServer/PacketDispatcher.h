#pragma once
#include "pch.h"

using namespace std;

using PacketHandler = bool(*)(CSession&, const char*, const PacketHeader&);

class CPacketDispatcher
{
public:
	bool Dispatch(CSession& _Session, const char* _Buffer, const PacketHeader& _Header) const;
	void RegisterAllHandlers();

private:
	void Register(PacketID _packetID, PacketHandler _handler);

private:
	array<PacketHandler, static_cast<size_t>(PacketID::ID_MAX)> m_Handlers{};
};