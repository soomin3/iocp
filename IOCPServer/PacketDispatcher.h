#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include "Packet.h"

class CSession;

using namespace std;

using PacketHandler = function<bool(CSession&, const char*, const PacketHeader&)>;

class CPacketDispatcher
{
public:
	bool Register(PacketID _PacketID, PacketHandler _Handler);
	bool Dispatch(CSession& _Session, const char* _Buffer, const PacketHeader& _Header) const;

private:
	array<PacketHandler, static_cast<size_t>(PacketID::ID_MAX)> m_Handlers{};
};