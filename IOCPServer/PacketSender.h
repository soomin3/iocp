#pragma once
#include <cstring>
#include <type_traits>
#include <vector>

using namespace std;

using PacketBuffer = vector<char>;

class IPacketSender
{
public:
	virtual ~IPacketSender() = default;
	virtual bool SendPacket(PacketBuffer _Packet) = 0;
};

template<typename PACKET>
PacketBuffer MakePacketBuffer(const PACKET& _Packet)
{
	static_assert(is_trivially_copyable_v<PACKET>);

	PacketBuffer Buffer(sizeof(PACKET));
	memcpy(Buffer.data(), &_Packet, sizeof(PACKET));
	return Buffer;
}
