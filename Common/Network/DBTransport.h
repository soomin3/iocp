#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include "Packet.h"

namespace DBTransport
{
	constexpr unsigned short PORT = 8695;
	constexpr DWORD TIMEOUT_MS = 5000;

	inline bool ConfigureSocket(SOCKET _Socket)
	{
		const BOOL NoDelay = TRUE;
		return setsockopt(_Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay)) == 0;
	}

	inline size_t PacketSize(PacketID _ID, bool _Request)
	{
		if (_Request)
		{
			if (_ID == PacketID::SD_LOGIN_REQ) return sizeof(SD_LOGIN_REQ);
			if (_ID == PacketID::SD_CREATE_NICKNAME_REQ) return sizeof(SD_CREATE_NICKNAME_REQ);
		}
		else
		{
			if (_ID == PacketID::DS_LOGIN_RES) return sizeof(DS_LOGIN_RES);
			if (_ID == PacketID::DS_CREATE_NICKNAME_RES) return sizeof(DS_CREATE_NICKNAME_RES);
		}
		return 0;
	}
}
