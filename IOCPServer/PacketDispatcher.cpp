#include "pch.h"
#include "PacketDispatcher.h"
#include "AuthHandlers.h"

void CPacketDispatcher::Register(PacketID _packetID, PacketHandler _handler)
{
	m_Handlers[static_cast<size_t>(_packetID)] = _handler;
}

bool CPacketDispatcher::Dispatch(CSession& _Session, const char* _Buffer, const PacketHeader& _Header) const
{
	size_t index = static_cast<size_t>(_Header.packetID);

	if (index >= m_Handlers.size() || index == static_cast<size_t>(PacketID::ID_NULL))
	{
		return false;
	}

	PacketHandler handler = m_Handlers[index];

	if (handler)
		return handler(_Session, _Buffer, _Header);

	return false;
}

void CPacketDispatcher::RegisterAllHandlers()
{
#define CS_PACKET(name) Register(PacketID::name, Handle_##name);
#define SC_PACKET(name) // 서버에서는 사용 안하는 패킷이라 더미만 생성
#include "PacketList.inc"
#undef CS_PACKET
#undef SC_PACKET
}
