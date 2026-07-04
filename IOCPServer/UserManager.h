#pragma once
#include "User.h"

using namespace std;

class CUserManager
{
public:

private:
	vector<unique_ptr<CUser>> m_UserPool;
};