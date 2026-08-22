#pragma once
#include <cstdint>

enum class ELoginResult : uint8_t
{
    SUCCESS,
    INVALID_CREDENTIAL,
    ALREADY_LOGGED_IN,
    DB_ERROR,
    SERVER_STOPPING
};

enum class ENicknameResult : uint8_t
{
    SUCCESS,
    DUPLICATED,
    ACCOUNT_NOT_FOUND,
    ALREADY_CREATED,
    INVALID_NICKNAME,
    DB_ERROR,
    INVALID_STATE
};

enum class EChatError : uint8_t
{
    NOT_READY,
    INVALID_MESSAGE,
    RATE_LIMITED,
    SERVER_BUSY,
    LOG_FAILURE
};
