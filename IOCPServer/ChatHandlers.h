#pragma once
#include "PacketDispatcher.h"

using namespace std;

class CWorldChat;

class CChatHandler final : public IPacketHandler
{
public:
	explicit CChatHandler(CWorldChat& _WorldChat) : m_WorldChat(_WorldChat) {}
	bool Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) override;

private:
	CWorldChat& m_WorldChat;
};

class CChatHandlers
{
public:
	explicit CChatHandlers(CWorldChat& _WorldChat) : m_ChatHandler(_WorldChat) {}
	bool RegisterAll(CPacketDispatcher& _PacketDispatcher);

private:
	CChatHandler m_ChatHandler;
};
