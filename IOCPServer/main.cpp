#include "pch.h"
#include "Server.h"

int main(int argc, char** argv)
{
	CServer Server;
	if (!Server.Init())
	{
		return -1;
	}

	Server.Run();

	return 0;
}
