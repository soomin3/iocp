#include "pch.h"
#include "AuthHandlers.h"

#include "DBManager.h"
#include "Packet.h"
#include "PacketSender.h"
#include "Session.h"
#include "TextValidation.h"
#include "User.h"
#include "UserManager.h"

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

	void DisconnectAuthenticatedSession(CSession& _Session, uint64_t _Generation, CUserManager& _UserManager)
	{
		if (_Session.GetGeneration() != _Generation)
		{
			return;
		}

		_Session.Disconnect();

		shared_ptr<CUser> DetachedUser = _Session.DetachUser();
		if (DetachedUser)
		{
			_UserManager.RemoveUser(DetachedUser->GetAccountID(), DetachedUser);
		}
	}
}

CAuthHandlers::CAuthHandlers(CDBManager& _DBManager, CUserManager& _UserManager)
	: m_LoginHandler(_DBManager),
	m_NicknameHandler(_DBManager),
	m_DSLoginHandler(_UserManager),
	m_DSCreateNicknameHandler(_UserManager)
{
}

bool CAuthHandlers::RegisterAll(CPacketDispatcher& _PacketDispatcher)
{
	if (!_PacketDispatcher.Register(PacketID::CS_LOGIN_REQ, m_LoginHandler))
	{
		return false;
	}

	if (!_PacketDispatcher.Register(PacketID::CS_CREATE_NICKNAME_REQ, m_NicknameHandler))
	{
		return false;
	}

	if (!_PacketDispatcher.Register(PacketID::DS_LOGIN_RES, m_DSLoginHandler))
	{
		return false;
	}

	if (!_PacketDispatcher.Register(PacketID::DS_CREATE_NICKNAME_RES, m_DSCreateNicknameHandler))
	{
		return false;
	}


	return true;
}

bool CLoginHandler::Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Context.Session == nullptr || _Header.packetSize != sizeof(CS_LOGIN_REQ))
	{
		return false;
	}

	CSession& Session = *_Context.Session;
	CS_LOGIN_REQ Packet{};
	memcpy(&Packet, _Buffer, sizeof(Packet));

	string ID;
	string PW;

	const uint64_t Generation = Session.GetGeneration();
	if (!ReadPacketString(Packet.id, ID) || !ReadPacketString(Packet.pw, PW) || ID.empty() || PW.empty())
	{
		SC_LOGIN_RES Response;
		Response.result = ELoginResult::INVALID_CREDENTIAL;
		return Session.Send(&Response, sizeof(Response), Generation);
	}

	if (!Session.TryBeginLogin(Generation))
	{
		return false;
	}

	SD_LOGIN_REQ Request;
	Request.sessionID = Session.GetSessionID();
	Request.generation = Generation;
	memcpy(Request.id, Packet.id, sizeof(Packet.id));
	memcpy(Request.pw, Packet.pw, sizeof(Packet.pw));

	if (!m_DBManager.Submit(MakePacketBuffer(Request)))
	{
		Session.CancelLogin(Generation);
		return false;
	}

	return true;
}

bool CNickNameHandler::Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Context.Session == nullptr || _Header.packetSize != sizeof(CS_CREATE_NICKNAME_REQ))
	{
		return false;
	}

	CSession& Session = *_Context.Session;
	CS_CREATE_NICKNAME_REQ Packet{};
	memcpy(&Packet, _Buffer, sizeof(Packet));

	const size_t NicknameLength = strnlen_s(Packet.nickname, sizeof(Packet.nickname));
	const uint64_t Generation = Session.GetGeneration();

	if (NicknameLength == sizeof(Packet.nickname))
	{
		SC_CREATE_NICKNAME_RES Response;
		Response.result = ENicknameResult::INVALID_NICKNAME;
		return Session.Send(&Response, sizeof(Response), Generation);
	}

	string Nickname(Packet.nickname, NicknameLength);
	if (!ValidateNickname(Nickname))
	{
		SC_CREATE_NICKNAME_RES Response;
		Response.result = ENicknameResult::INVALID_NICKNAME;
		return Session.Send(&Response, sizeof(Response), Generation);
	}

	shared_ptr<CUser> User = Session.GetUser();
	if (!User || !Session.IsAuthenticated(User, Generation))
	{
		SC_CREATE_NICKNAME_RES Response;
		Response.result = ENicknameResult::INVALID_STATE;
		return Session.Send(&Response, sizeof(Response), Generation);
	}

	const uint64_t RequestID = m_DBManager.CreateRequestID();
	if (!User->TryBeginNicknameRequest(RequestID))
	{
		SC_CREATE_NICKNAME_RES Response;
		Response.result = ENicknameResult::INVALID_STATE;
		return Session.Send(&Response, sizeof(Response), Generation);
	}

	SD_CREATE_NICKNAME_REQ Request;
	Request.requestID = RequestID;
	Request.accountID = User->GetAccountID();
	memcpy(Request.nickname, Nickname.data(), Nickname.size());

	if (!m_DBManager.Submit(MakePacketBuffer(Request)))
	{
		User->FinishDBRequest(RequestID);

		SC_CREATE_NICKNAME_RES Response;
		Response.result = ENicknameResult::DB_ERROR;
		return Session.Send(&Response, sizeof(Response), Generation);
	}

	return true;
}

bool CDSLoginHandler::Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Context.Session == nullptr || _Header.packetSize != sizeof(DS_LOGIN_RES))
	{
		return false;
	}

	DS_LOGIN_RES Packet;
	memcpy(&Packet, _Buffer, sizeof(Packet));

	CSession* Session = _Context.Session;
	if (Session->GetSessionID() != Packet.sessionID)
	{
		return true;
	}

	const uint64_t Generation = Packet.generation;
	if (!Session->IsLoginPending(Generation))
	{
		return true;
	}

	ELoginResult LoginResult = ELoginResult::DB_ERROR;
	shared_ptr<CUser> User;
	string Nickname;

	const bool ValidNickname = Packet.hasNickname == 0 ||
		(ReadPacketString(Packet.nickname, Nickname) && ValidateNickname(Nickname));

	if (Packet.dbSucceeded == 0 || !ValidNickname)
	{
		Session->CancelLogin(Generation);
	}
	else if (Packet.resultCode != 0 || Packet.hasAccountID == 0)
	{
		LoginResult = ELoginResult::INVALID_CREDENTIAL;
		Session->CancelLogin(Generation);
	}
	else if (Packet.accountID <= 0)
	{
		Session->CancelLogin(Generation);
	}
	else
	{
		User = m_UserManager.TryAddUser(Packet.accountID, *Session, Generation);

		if (!User)
		{
			LoginResult = ELoginResult::ALREADY_LOGGED_IN;
			Session->CancelLogin(Generation);
		}
		else
		{
			const bool HasNickname = Packet.hasNickname != 0;

			if (HasNickname && !User->SetNicknameOnce(Nickname))
			{
				m_UserManager.RemoveUser(Packet.accountID, User);
				User.reset();
				Session->CancelLogin(Generation);
			}
			else if (!Session->CompleteLogin(User, Generation))
			{
				m_UserManager.RemoveUser(Packet.accountID, User);
				User.reset();
				Session->CancelLogin(Generation);
			}
			else
			{
				LoginResult = ELoginResult::SUCCESS;
			}
		}
	}

	SC_LOGIN_RES Response;
	Response.result = LoginResult;
	Response.needNickname = LoginResult == ELoginResult::SUCCESS && User && !User->HasNickname();

	if (!Session->Send(&Response, sizeof(Response), Generation))
	{
		DisconnectAuthenticatedSession(*Session, Generation, m_UserManager);
	}

	return true;
}

bool CDSCreateNicknameHandler::Handle(const SPacketContext&, const char* _Buffer, const PacketHeader& _Header)
{
	if (_Header.packetSize != sizeof(DS_CREATE_NICKNAME_RES))
	{
		return false;
	}

	DS_CREATE_NICKNAME_RES Packet;
	memcpy(&Packet, _Buffer, sizeof(Packet));

	SActiveUserContext UserContext;
	if (Packet.accountID <= 0 || !m_UserManager.FindUser(Packet.accountID, UserContext) ||
		!UserContext.User)
	{
		return true;
	}

	shared_ptr<CUser> User = UserContext.User;

	ENicknameResult Result = ENicknameResult::DB_ERROR;
	if (Packet.dbSucceeded != 0)
	{
		switch (Packet.resultCode)
		{
		case 0:
			Result = ENicknameResult::SUCCESS;
			break;
		case 1:
			Result = ENicknameResult::DUPLICATED;
			break;
		case 2:
			Result = ENicknameResult::ACCOUNT_NOT_FOUND;
			break;
		case 3:
			Result = ENicknameResult::ALREADY_CREATED;
			break;
		case 4:
			Result = ENicknameResult::INVALID_NICKNAME;
			break;
		default:
			Result = ENicknameResult::DB_ERROR;
			break;
		}
	}

	if (Result == ENicknameResult::SUCCESS)
	{
		string Nickname;

		if (!ReadPacketString(Packet.nickname, Nickname) || !ValidateNickname(Nickname))
		{
			if (!User->FinishDBRequest(Packet.requestID))
				return true;

			Result = ENicknameResult::DB_ERROR;
		}
		else if (!User->CompleteNicknameRequest(Packet.requestID, Nickname))
		{
			return true;
		}
	}
	else if (!User->FinishDBRequest(Packet.requestID))
	{
		return true;
	}

	CSession* Session = UserContext.Session;
	if (Session == nullptr || !Session->IsAuthenticated(User, UserContext.Generation))
	{
		return true;
	}

	SC_CREATE_NICKNAME_RES Response;
	Response.result = Result;

	if (!Session->Send(&Response, sizeof(Response), UserContext.Generation))
	{
		DisconnectAuthenticatedSession(*Session, UserContext.Generation, m_UserManager);
	}

	return true;
}
