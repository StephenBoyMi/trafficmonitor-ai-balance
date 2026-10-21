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

// 控制台类接口用浏览器 Cookie 鉴权：origin 同时作为 Origin 头和 Referer。
HttpResult HttpGetWithCookie(const std::wstring& url, const std::string& cookie,
    const std::wstring& origin);
HttpResult HttpPostForm(const std::wstring& url, const std::string& body,
    const std::string& cookie, const std::wstring& origin);
