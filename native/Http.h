#pragma once
#include <string>

struct HttpResult
{
    int status;
    std::string body;
    std::string error;

    HttpResult() : status(0) {}
};

HttpResult HttpGet(const std::wstring& url, const std::string& apiKey);
