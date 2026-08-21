#pragma once

class CDBManager;
class CPacketDispatcher;

bool RegisterAuthHandlers(CPacketDispatcher& _PacketDispatcher, CDBManager& _DBManager);
