#pragma once
#include "Packet.h"

using namespace std;

class CDBConnection;
class IPacketSender;

class IDBRequestHandler
{
public:
	virtual ~IDBRequestHandler() = default;
	virtual bool Handle(CDBConnection& _Connection, IPacketSender& _ResponseSender, const char* _Buffer, const PacketHeader& _Header) = 0;
};

class CDBLoginRequestHandler final : public IDBRequestHandler
{
public:
	bool Handle(CDBConnection& _Connection, IPacketSender& _ResponseSender, const char* _Buffer, const PacketHeader& _Header) override;
};

class CDBCreateNicknameRequestHandler final : public IDBRequestHandler
{
public:
	bool Handle(CDBConnection& _Connection, IPacketSender& _ResponseSender, const char* _Buffer, const PacketHeader& _Header) override;
};
