#include "IOCPManager.h"

int main(int argc, char** argv)
{
	CIOCPManager iocpManager;
	iocpManager.Init();
	iocpManager.Run();

	return 0;
}