#include "pch.h"
#include "ChatHandlers.h"

#include "WorldChat.h"
#include "Packet.h"
#include "Session.h"
#include "TextValidation.h"
#include "User.h"

namespace
{
	bool SendChatError(CSession& _Session, uint64_t _Generation, EChatError _Error)
	{
		SC_CHAT_ERROR Packet;
		Packet.error = _Error;
		return _Session.Send(&Packet, sizeof(Packet), _Generation);
	}
}

bool CChatHandlers::RegisterAll(CPacketDispatcher& _PacketDispatcher)
{
	return _PacketDispatcher.Register(PacketID::CS_CHAT_REQ, m_ChatHandler);
}

bool CChatHandler::Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Context.Session == nullptr || _Header.packetSize != sizeof(CS_CHAT_REQ))
	{
		return false;
	}

	CSession& Session = *_Context.Session;

	const uint64_t Generation = Session.GetGeneration();
	shared_ptr<CUser> User = Session.GetUser();
	if (!User || !Session.IsAuthenticated(User, Generation) || !User->HasNickname())
	{
		return SendChatError(Session, Generation, EChatError::NOT_READY);
	}

	if (!User->TryConsumeChatQuota())
	{
		return SendChatError(Session, Generation, EChatError::RATE_LIMITED);
	}

	CS_CHAT_REQ Packet{};
	memcpy(&Packet, _Buffer, sizeof(Packet));

	const size_t MessageLength = strnlen_s(Packet.message, sizeof(Packet.message));
	if (MessageLength == sizeof(Packet.message))
	{
		return SendChatError(Session, Generation, EChatError::INVALID_MESSAGE);
	}

	string Message(Packet.message, MessageLength);
	if (!ValidateChatMessage(Message))
	{
		return SendChatError(Session, Generation, EChatError::INVALID_MESSAGE);
	}

	if (!m_WorldChat.Submit(User, Session, Generation, Message))
	{
		return SendChatError(Session, Generation, EChatError::SERVER_BUSY);
	}

	return true;
}
