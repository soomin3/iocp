#include "pch.h"
#include "TextEncoding.h"

bool Utf8ToWide(const string& _Source, wstring& _Destination)
{
	if (_Source.empty())
	{
		_Destination.clear();
		return true;
	}

	const int Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _Source.data(), static_cast<int>(_Source.size()), nullptr, 0);
	if (Length <= 0)
	{
		return false;
	}

	_Destination.resize(static_cast<size_t>(Length));
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _Source.data(), static_cast<int>(_Source.size()), _Destination.data(), Length) == Length;
}

bool WideToUtf8(const std::wstring& _Source, std::string& _Destination)
{
	if (_Source.empty())
	{
		_Destination.clear();
		return true;
	}

	const int Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, _Source.data(), static_cast<int>(_Source.size()), nullptr, 0, nullptr, nullptr);
	if (Length <= 0)
	{
		return false;
	}

	_Destination.resize(static_cast<size_t>(Length));
	return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, _Source.data(), static_cast<int>(_Source.size()), _Destination.data(), Length, nullptr, nullptr) == Length;
}
