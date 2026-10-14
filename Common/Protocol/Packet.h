#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>
#include "Myinclude.h"

using namespace std;

using PacketBuffer = vector<char>;

template<typename PACKET>
PacketBuffer MakePacketBuffer(const PACKET& _Packet)
{
	static_assert(is_trivially_copyable_v<PACKET>);

	PacketBuffer Buffer(sizeof(PACKET));
	memcpy(Buffer.data(), &_Packet, sizeof(PACKET));
	return Buffer;
}

enum class PacketID : uint16_t
{
	ID_NULL = 0,

#define PACKET(name) name,
#include "PacketList.inc"
#undef PACKET
	
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
	SC_LOGIN_RES() : result(ELoginResult::DB_ERROR), needNickname(0), reserved(0)
	{
		packetID = PacketID::SC_LOGIN_RES;
		packetSize = sizeof(SC_LOGIN_RES);
	}

	ELoginResult result;
	uint8_t needNickname;
	uint16_t reserved;
};

struct CS_CREATE_NICKNAME_REQ : public PacketHeader
{
	CS_CREATE_NICKNAME_REQ()
	{
		packetID = PacketID::CS_CREATE_NICKNAME_REQ;
		packetSize = sizeof(CS_CREATE_NICKNAME_REQ);
		memset(nickname, 0, sizeof(nickname));
	}

	char nickname[64];
};

struct SC_CREATE_NICKNAME_RES : public PacketHeader
{
	SC_CREATE_NICKNAME_RES() : result(ENicknameResult::DB_ERROR), reserved(0)
	{
		packetID = PacketID::SC_CREATE_NICKNAME_RES;
		packetSize = sizeof(SC_CREATE_NICKNAME_RES);
	}

	ENicknameResult result;
	uint8_t reserved;
};

struct CS_CHAT_REQ : public PacketHeader
{
	CS_CHAT_REQ()
	{
		packetID = PacketID::CS_CHAT_REQ;
		packetSize = sizeof(CS_CHAT_REQ);
		memset(message, 0, sizeof(message));
	}

	char message[256];
};

struct SC_CHAT_BROADCAST : public PacketHeader
{
	SC_CHAT_BROADCAST() : reserved(0), sequence(0), utcMilliseconds(0), accountID(0)
	{
		packetID = PacketID::SC_CHAT_BROADCAST;
		packetSize = sizeof(SC_CHAT_BROADCAST);

		memset(nickname, 0, sizeof(nickname));
		memset(message, 0, sizeof(message));
	}

	uint32_t reserved;
	uint64_t sequence;
	int64_t utcMilliseconds;
	int64_t accountID;
	char nickname[64];
	char message[256];
};

struct SC_CHAT_ERROR : public PacketHeader
{
	SC_CHAT_ERROR() : error(EChatError::SERVER_BUSY), reserved{}
	{
		packetID = PacketID::SC_CHAT_ERROR;
		packetSize = sizeof(SC_CHAT_ERROR);
	}

	EChatError error;
	uint8_t reserved[3];
};

struct SD_LOGIN_REQ : public PacketHeader
{
	SD_LOGIN_REQ() : sessionID(0), generation(0)
	{
		packetID = PacketID::SD_LOGIN_REQ;
		packetSize = sizeof(SD_LOGIN_REQ);
		memset(id, 0, sizeof(id));
		memset(pw, 0, sizeof(pw));
	}

	uint32_t sessionID;
	uint64_t generation;
	char id[32];
	char pw[32];
};

struct DS_LOGIN_RES : public PacketHeader
{
	DS_LOGIN_RES() : sessionID(0), generation(0), accountID(0), resultCode(-1), dbSucceeded(0), hasAccountID(0), hasNickname(0), reserved(0)
	{
		packetID = PacketID::DS_LOGIN_RES;
		packetSize = sizeof(DS_LOGIN_RES);
		memset(nickname, 0, sizeof(nickname));
	}

	uint32_t sessionID;
	uint64_t generation;
	int64_t accountID;
	int32_t resultCode;
	uint8_t dbSucceeded;
	uint8_t hasAccountID;
	uint8_t hasNickname;
	uint8_t reserved;
	char nickname[65];
};

struct SD_CREATE_NICKNAME_REQ : public PacketHeader
{
	SD_CREATE_NICKNAME_REQ() : requestID(0), accountID(0)
	{
		packetID = PacketID::SD_CREATE_NICKNAME_REQ;
		packetSize = sizeof(SD_CREATE_NICKNAME_REQ);
		memset(nickname, 0, sizeof(nickname));
	}

	uint64_t requestID;
	int64_t accountID;
	char nickname[64];
};

struct DS_CREATE_NICKNAME_RES : public PacketHeader
{
	DS_CREATE_NICKNAME_RES() : requestID(0), accountID(0), resultCode(-1), dbSucceeded(0), reserved{}
	{
		packetID = PacketID::DS_CREATE_NICKNAME_RES;
		packetSize = sizeof(DS_CREATE_NICKNAME_RES);
		memset(nickname, 0, sizeof(nickname));
	}

	uint64_t requestID;
	int64_t accountID;
	int32_t resultCode;
	uint8_t dbSucceeded;
	uint8_t reserved[3];
	char nickname[64];
};

static_assert(sizeof(PacketHeader) == 4);
static_assert(sizeof(CS_LOGIN_REQ) == 68);
static_assert(sizeof(SC_LOGIN_RES) == 8);
static_assert(sizeof(CS_CREATE_NICKNAME_REQ) == 68);
static_assert(sizeof(SC_CREATE_NICKNAME_RES) == 6);
static_assert(sizeof(CS_CHAT_REQ) == 260);
static_assert(sizeof(SC_CHAT_BROADCAST) == 352);
static_assert(sizeof(SC_CHAT_ERROR) == 8);
static_assert(sizeof(SD_LOGIN_REQ) == 80);
static_assert(sizeof(DS_LOGIN_RES) == 104);
static_assert(sizeof(SD_CREATE_NICKNAME_REQ) == 88);
static_assert(sizeof(DS_CREATE_NICKNAME_RES) == 96);
