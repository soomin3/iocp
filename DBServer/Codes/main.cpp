#include "OLEDBDatabase.h"
#include "DBServer.h"

#include <iostream>
#include <memory>
#include <string>

int main()
{
	CDBServer Server;
	if (!Server.Start([]() -> CDBServer::RequestHandler
		{
			auto Database = std::make_shared<COLEDBDatabase>();
			if (!Database->Open())
			{
				return {};
			}
			return [Database](const PacketBuffer& _Request) { return Database->Handle(_Request); };
		}))
	{
		std::cerr << "DBServer start failed\n";
		return 1;
	}
	std::cout << "DBServer is running on 127.0.0.1:" << DBTransport::PORT
		<< " (MSOLEDBSQL19)\nCommands: help, status, quit\n";
	std::string Command;
	while (getline(std::cin, Command))
	{
		if (Command == "quit")
		{
			break;
		}
		if (Command == "status")
		{
			Server.PrintStatus();
		}
		else if (Command == "help")
		{
			std::cout << "help - Show commands\nstatus - Show DBServer status\nquit - Stop DBServer\n";
		}
		else if (!Command.empty())
		{
			std::cout << "Unknown command: " << Command << '\n';
		}
	}
	Server.Stop();
	return 0;
}
