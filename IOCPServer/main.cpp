#include "pch.h"
#include "Server.h"

int main()
{
	CServer Server;
	if (!Server.Init())
	{
		return -1;
	}

	Server.Run();

	return 0;
}
