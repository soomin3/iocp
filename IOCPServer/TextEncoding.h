#pragma once
#include <string>

using namespace std;

bool Utf8ToWide(const string& _Source, wstring& _Destination);
bool WideToUtf8(const wstring& _Source, string& _Destination);
