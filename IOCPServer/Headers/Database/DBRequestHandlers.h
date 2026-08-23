#pragma once
#include "Packet.h"

using namespace std;

class CDBConnection;

class IDBRequestHandler
{
public:
	virtual ~IDBRequestHandler() = default;
	virtual PacketBuffer Handle(CDBConnection& _Connection, const char* _Buffer, const PacketHeader& _Header) = 0;
};

class CDBLoginRequestHandler final : public IDBRequestHandler
{
public:
	PacketBuffer Handle(CDBConnection& _Connection, const char* _Buffer, const PacketHeader& _Header) override;
};

class CDBCreateNicknameRequestHandler final : public IDBRequestHandler
{
public:
	PacketBuffer Handle(CDBConnection& _Connection, const char* _Buffer, const PacketHeader& _Header) override;
};
