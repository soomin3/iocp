#include "pch.h"
#include "DBRequestHandlers.h"
#include "AuthProcedures.h"
#include "DBConnection.h"

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

PacketBuffer CDBLoginRequestHandler::Handle(CDBConnection& _Connection, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Buffer == nullptr || _Header.packetSize != sizeof(SD_LOGIN_REQ))
	{
		return {};
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
		return MakePacketBuffer(Response);
	}

	SDBUserLoginResult Result;
	if (ExecuteUserLogin(_Connection, move(ID), move(PW), Result))
	{
		Response.resultCode = Result.ResultCode;
		Response.hasAccountID = Result.HasAccountID ? 1 : 0;
		Response.accountID = Result.AccountID;

		if (Result.HasNickname)
		{
			if (!WritePacketString(Result.Nickname, Response.nickname))
			{
				return MakePacketBuffer(Response);
			}

			Response.hasNickname = 1;
		}

		Response.dbSucceeded = 1;
	}

	return MakePacketBuffer(Response);
}

PacketBuffer CDBCreateNicknameRequestHandler::Handle(CDBConnection& _Connection, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Buffer == nullptr || _Header.packetSize != sizeof(SD_CREATE_NICKNAME_REQ))
	{
		return {};
	}

	SD_CREATE_NICKNAME_REQ Request;
	memcpy(&Request, _Buffer, sizeof(Request));

	DS_CREATE_NICKNAME_RES Response;
	Response.requestID = Request.requestID;
	Response.accountID = Request.accountID;

	string Nickname;
	if (!ReadPacketString(Request.nickname, Nickname) || Nickname.empty() || !WritePacketString(Nickname, Response.nickname))
	{
		return MakePacketBuffer(Response);
	}

	SDBCreateNicknameResult Result;
	if (ExecuteCreateNickname(_Connection, Request.accountID, move(Nickname), Result))
	{
		Response.resultCode = Result.ResultCode;
		Response.dbSucceeded = 1;
	}

	return MakePacketBuffer(Response);
}
