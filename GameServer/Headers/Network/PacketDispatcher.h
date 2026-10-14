#pragma once
#include <array>
#include <cstddef>
#include "Packet.h"

using namespace std;

class CSession;

enum class EPacketSource : uint8_t
{
	CLIENT,
	DATABASE,
	GAME_SERVER
};

struct SPacketContext
{
	EPacketSource Source = EPacketSource::CLIENT;
	CSession* Session = nullptr;
};

class IPacketHandler
{
public:
	virtual ~IPacketHandler() = default;
	virtual bool Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) = 0;
};

class CPacketDispatcher
{
public:
	bool Register(PacketID _PacketID, IPacketHandler& _Handler);
	bool Dispatch(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) const;

private:
	array<IPacketHandler*, static_cast<size_t>(PacketID::ID_MAX)> m_Handlers{};
};
