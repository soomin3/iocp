#include "pch.h"
#include "PacketDispatcher.h"

namespace
{
	bool IsAllowedPacket(EPacketSource _Source, PacketID _PacketID)
	{
		switch (_Source)
		{
		case EPacketSource::CLIENT:
			return _PacketID == PacketID::CS_LOGIN_REQ || _PacketID == PacketID::CS_CREATE_NICKNAME_REQ || _PacketID == PacketID::CS_CHAT_REQ;
		case EPacketSource::DATABASE:
			return _PacketID == PacketID::DS_LOGIN_RES || _PacketID == PacketID::DS_CREATE_NICKNAME_RES;
		case EPacketSource::GAME_SERVER:
			return _PacketID == PacketID::SD_LOGIN_REQ || _PacketID == PacketID::SD_CREATE_NICKNAME_REQ;
		default:
			return false;
		}
	}
}

bool CPacketDispatcher::Register(PacketID _PacketID, IPacketHandler& _Handler)
{
	const size_t index = static_cast<size_t>(_PacketID);

	if (index == static_cast<size_t>(PacketID::ID_NULL) || index >= m_Handlers.size())
	{
		return false;
	}

	if (m_Handlers[index])
	{
		return false;
	}

	m_Handlers[index] = &_Handler;

	return true;
}

bool CPacketDispatcher::Dispatch(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) const
{
	if (_Buffer == nullptr || (_Context.Source == EPacketSource::CLIENT && _Context.Session == nullptr) || !IsAllowedPacket(_Context.Source, _Header.packetID))
	{
		return false;
	}

	const size_t index = static_cast<size_t>(_Header.packetID);

	if (index >= m_Handlers.size() || index == static_cast<size_t>(PacketID::ID_NULL))
	{
		return false;
	}

	IPacketHandler* Handler = m_Handlers[index];

	if (Handler)
	{
		return Handler->Handle(_Context, _Buffer, _Header);
	}

	return false;
}
