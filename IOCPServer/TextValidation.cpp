#include "pch.h"
#include "TextValidation.h"
#include "TextEncoding.h"

#include <cwctype>

namespace
{
	bool CountCodePoints(const wstring& _Text, size_t& _Count)
	{
		_Count = 0;

		for (size_t Index = 0; Index < _Text.size(); ++Index)
		{
			const wchar_t Character = _Text[Index];

			if (Character >= 0xD800 && Character <= 0xDBFF)
			{
				if (Index + 1 >= _Text.size() || _Text[Index + 1] < 0xDC00 || _Text[Index + 1] > 0xDFFF)
				{
					return false;
				}

				++Index;
			}
			else if (Character >= 0xDC00 && Character <= 0xDFFF)
			{
				return false;
			}

			++_Count;
		}

		return true;
	}
}

bool ValidateNickname(const string& _Nickname)
{
	if (_Nickname.empty() || _Nickname.size() >= 64)
	{
		return false;
	}

	wstring WideNickname;
	if (!Utf8ToWide(_Nickname, WideNickname) || WideNickname.empty())
	{
		return false;
	}

	if (iswspace(WideNickname.front()) || iswspace(WideNickname.back()))
	{
		return false;
	}

	for (const wchar_t Character : WideNickname)
	{
		if (iswcntrl(Character))
		{
			return false;
		}
	}

	size_t CodePointCount = 0;
	return CountCodePoints(WideNickname, CodePointCount) && CodePointCount <= 16;
}

bool ValidateChatMessage(const string& _Message)
{
	if (_Message.empty() || _Message.size() >= 256)
	{
		return false;
	}

	wstring WideMessage;
	if (!Utf8ToWide(_Message, WideMessage) || WideMessage.empty())
	{
		return false;
	}

	for (const wchar_t Character : WideMessage)
	{
		if (iswcntrl(Character))
		{
			return false;
		}
	}

	return true;
}
