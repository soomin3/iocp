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
		const DWORD Timeout = TIMEOUT_MS;
		const BOOL NoDelay = TRUE;
		return setsockopt(_Socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&Timeout), sizeof(Timeout)) == 0 &&
			setsockopt(_Socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&Timeout), sizeof(Timeout)) == 0 &&
			setsockopt(_Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay)) == 0;
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

	inline bool SendPacket(SOCKET _Socket, const PacketBuffer& _Packet)
	{
		size_t Offset = 0;
		while (Offset < _Packet.size())
		{
			const int Sent = send(_Socket, _Packet.data() + Offset, static_cast<int>(_Packet.size() - Offset), 0);
			if (Sent <= 0) return false;
			Offset += static_cast<size_t>(Sent);
		}
		return true;
	}

	inline bool ReceiveBytes(SOCKET _Socket, char* _Buffer, size_t _Size, ULONGLONG _Deadline)
	{
		size_t Offset = 0;
		while (Offset < _Size)
		{
			const ULONGLONG Now = GetTickCount64();
			if (Now >= _Deadline) return false;
			const long Remaining = static_cast<long>(_Deadline - Now);
			timeval Timeout{ Remaining / 1000, (Remaining % 1000) * 1000 };
			fd_set ReadSet;
			FD_ZERO(&ReadSet);
			FD_SET(_Socket, &ReadSet);
			if (select(0, &ReadSet, nullptr, nullptr, &Timeout) <= 0) return false;
			const int Received = recv(_Socket, _Buffer + Offset, static_cast<int>(_Size - Offset), 0);
			if (Received <= 0) return false;
			Offset += static_cast<size_t>(Received);
		}
		return true;
	}

	inline bool ReceivePacket(SOCKET _Socket, PacketBuffer& _Packet, bool _Request)
	{
		const ULONGLONG Deadline = GetTickCount64() + TIMEOUT_MS;
		PacketHeader Header;
		if (!ReceiveBytes(_Socket, reinterpret_cast<char*>(&Header), sizeof(Header), Deadline)) return false;
		const size_t Size = PacketSize(Header.packetID, _Request);
		if (Size == 0 || Header.packetSize != Size) return false;
		_Packet.resize(Size);
		memcpy(_Packet.data(), &Header, sizeof(Header));
		return ReceiveBytes(_Socket, _Packet.data() + sizeof(Header), Size - sizeof(Header), Deadline);
	}
}
