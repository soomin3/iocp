#include "IOCPManager.h"

int main(int argc, char** argv)
{
	CIOCPManager iocpManager;
	iocpManager.Init();
	iocpManager.Run();
	//iocpManager.Release();
	return 0;
}