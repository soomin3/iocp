#pragma once
#include "PacketDispatcher.h"
#include "Packet.h"

class CDBManager;
class CUserManager;

class CLoginHandler final : public IPacketHandler
{
public:
	explicit CLoginHandler(CDBManager& _DBManager) : m_DBManager(_DBManager) {}
	bool Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) override;

private:
	CDBManager& m_DBManager;
};

class CNickNameHandler final : public IPacketHandler
{
public:
	explicit CNickNameHandler(CDBManager& _DBManager) : m_DBManager(_DBManager) {}
	bool Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) override;

private:
	CDBManager& m_DBManager;
};

class CDSLoginHandler final : public IPacketHandler
{
public:
	explicit CDSLoginHandler(CUserManager& _UserManager) : m_UserManager(_UserManager) {}
	bool Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) override;

private:
	CUserManager& m_UserManager;
};

class CDSCreateNicknameHandler final : public IPacketHandler
{
public:
	explicit CDSCreateNicknameHandler(CUserManager& _UserManager) : m_UserManager(_UserManager) {}
	bool Handle(const SPacketContext& _Context, const char* _Buffer, const PacketHeader& _Header) override;

private:
	CUserManager& m_UserManager;
};

class CAuthHandlers
{
public:
	CAuthHandlers(CDBManager& _DBManager, CUserManager& _UserManager);
	bool RegisterAll(CPacketDispatcher& _PacketDispatcher);

private:
	CLoginHandler m_LoginHandler;
	CNickNameHandler m_NicknameHandler;
	CDSLoginHandler m_DSLoginHandler;
	CDSCreateNicknameHandler m_DSCreateNicknameHandler;
};
