#pragma once

enum class PacketID : uint16_t
{
	ID_NULL = 0,

#define CS_PACKET(name) name,
#define SC_PACKET(name) name,
#include "PacketList.inc"
#undef CS_PACKET
#undef SC_PACKET
	
	ID_MAX
};

struct PacketHeader
{
	PacketHeader() : packetSize(0), packetID(PacketID::ID_NULL)
	{
	}
	uint16_t packetSize;
	PacketID packetID;
};

struct CS_LOGIN_REQ : public PacketHeader
{
	CS_LOGIN_REQ()
	{
		packetID = PacketID::CS_LOGIN_REQ;
		packetSize = sizeof(CS_LOGIN_REQ);
		memset(id, 0, sizeof(id));
		memset(pw, 0, sizeof(pw));
	}
	char id[32];
	char pw[32];
};

struct SC_LOGIN_RES : public PacketHeader
{
	SC_LOGIN_RES() : result(false)
	{
		packetID = PacketID::SC_LOGIN_RES;
		packetSize = sizeof(SC_LOGIN_RES);
	}
	bool result;
};
