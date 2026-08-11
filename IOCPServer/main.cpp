#include "pch.h"
#include "IOCPManager.h"
#include "DBManager.h"

int main(int argc, char** argv)
{
	CDBManager DBManager;
	bool Result = DBManager.Init();
	if (!Result)
	{
		cout << "SQL Server Init Failed" << endl;
		return -1;
	}
	cout << "SQL Server Init Success" << endl;

	//Result = DBManager.Connect(L"Driver={SQL Server};Server=127.0.0.1;Database=TestDB;Trusted_Connection=yes;");
	Result = DBManager.Connect(L"Driver={SQL Server};Server=127.0.0.1,1433;Database=TestDB;UID = sa;PWD = test;TrustedServerCertificate=yes;");
	if (!Result)
	{
		cout << "SQL Server Connect Failed" << endl;
		return -1;
	}
	cout << "SQL Server Connect Success" << endl;

	CIOCPManager iocpManager;
	iocpManager.Init();
	iocpManager.Run();

	return 0;
}