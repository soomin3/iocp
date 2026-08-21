#include "pch.h"
#include "PacketDispatcher.h"

bool CPacketDispatcher::Register(PacketID _PacketID, PacketHandler _Handler)
{
	const size_t index = static_cast<size_t>(_PacketID);

	if (index == static_cast<size_t>(PacketID::ID_NULL) || index >= m_Handlers.size() || !_Handler)
	{
		return false;
	}

	if (m_Handlers[index])
	{
		return false;
	}

	m_Handlers[index] = move(_Handler);

	return true;
}

bool CPacketDispatcher::Dispatch(CSession& _Session, const char* _Buffer, const PacketHeader& _Header) const
{
	size_t index = static_cast<size_t>(_Header.packetID);

	if (index >= m_Handlers.size() || index == static_cast<size_t>(PacketID::ID_NULL))
	{
		return false;
	}

	const PacketHandler& handler = m_Handlers[index];

	if (handler)
		return handler(_Session, _Buffer, _Header);

	return false;
}
