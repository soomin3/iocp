#include "pch.h"
#include "DBRequestHandlers.h"
#include "AuthProcedures.h"
#include "DBConnection.h"
#include "PacketSender.h"

namespace
{
	template<size_t Size>
	bool ReadPacketString(const char (&_Source)[Size], string& _Value)
	{
		const size_t Length = strnlen_s(_Source, Size);
		if (Length == Size)
		{
			return false;
		}

		_Value.assign(_Source, Length);
		return true;
	}

	template<size_t Size>
	bool WritePacketString(const string& _Value, char (&_Destination)[Size])
	{
		if (_Value.size() >= Size)
		{
			return false;
		}

		memcpy(_Destination, _Value.data(), _Value.size());
		return true;
	}
}

bool CDBLoginRequestHandler::Handle(CDBConnection& _Connection, IPacketSender& _ResponseSender, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Buffer == nullptr || _Header.packetSize != sizeof(SD_LOGIN_REQ))
	{
		return false;
	}

	SD_LOGIN_REQ Request;
	memcpy(&Request, _Buffer, sizeof(Request));

	DS_LOGIN_RES Response;
	Response.sessionID = Request.sessionID;
	Response.generation = Request.generation;

	string ID;
	string PW;
	if (!ReadPacketString(Request.id, ID) || !ReadPacketString(Request.pw, PW) || ID.empty() || PW.empty())
	{
		return _ResponseSender.SendPacket(MakePacketBuffer(Response));
	}

	CProcedure_UserLogin Procedure;
	Procedure.ID = move(ID);
	Procedure.PW = move(PW);
	Procedure.Execute(_Connection);

	if (Procedure.Succeeded())
	{
		Response.resultCode = Procedure.ResultCode.Value;
		Response.hasAccountID = Procedure.AccountID.IsNull() ? 0 : 1;
		Response.accountID = Response.hasAccountID == 0 ? 0 : Procedure.AccountID.Value;

		if (!Procedure.Nickname.IsNull() && !Procedure.Nickname.Value.empty())
		{
			if (!WritePacketString(Procedure.Nickname.Value, Response.nickname))
			{
				return _ResponseSender.SendPacket(MakePacketBuffer(Response));
			}

			Response.hasNickname = 1;
		}

		Response.dbSucceeded = 1;
	}

	return _ResponseSender.SendPacket(MakePacketBuffer(Response));
}

bool CDBCreateNicknameRequestHandler::Handle(CDBConnection& _Connection, IPacketSender& _ResponseSender, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Buffer == nullptr || _Header.packetSize != sizeof(SD_CREATE_NICKNAME_REQ))
	{
		return false;
	}

	SD_CREATE_NICKNAME_REQ Request;
	memcpy(&Request, _Buffer, sizeof(Request));

	DS_CREATE_NICKNAME_RES Response;
	Response.requestID = Request.requestID;
	Response.accountID = Request.accountID;

	string Nickname;
	if (!ReadPacketString(Request.nickname, Nickname) ||
		Nickname.empty() || !WritePacketString(Nickname, Response.nickname))
	{
		return _ResponseSender.SendPacket(MakePacketBuffer(Response));
	}

	CProcedure_CreateNickname Procedure;
	Procedure.AccountID = Request.accountID;
	Procedure.Nickname = Nickname;
	Procedure.Execute(_Connection);

	if (Procedure.Succeeded())
	{
		Response.resultCode = Procedure.ResultCode.Value;
		Response.dbSucceeded = 1;
	}

	return _ResponseSender.SendPacket(MakePacketBuffer(Response));
}
