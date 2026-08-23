#pragma once
#include "Packet.h"

using namespace std;

class IPacketSender
{
public:
	virtual ~IPacketSender() = default;
	virtual bool SendPacket(PacketBuffer _Packet) = 0;
};
