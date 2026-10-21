#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include "Http.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace
{
    std::string LastErrorText(const char* prefix)
    {
        DWORD code = GetLastError();
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s（WinHTTP %lu）", prefix,
            static_cast<unsigned long>(code));
        return std::string(buffer);
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return std::wstring();
        }
        int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
        {
            return std::wstring();
        }
        std::wstring result;
        result.resize(static_cast<size_t>(length));
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), &result[0], length);
        return result;
    }

    void CloseInternet(HINTERNET handle)
    {
        if (handle != nullptr)
        {
            WinHttpCloseHandle(handle);
        }
    }

    HttpResult HttpPerform(const std::wstring& url, const wchar_t* method,
        const std::string* body, const std::wstring& headers,
        const std::wstring& referer)
    {
        HttpResult result;

        URL_COMPONENTS components;
        ZeroMemory(&components, sizeof(components));
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components))
        {
            result.error = LastErrorText("解析 URL 失败");
            return result;
        }

        std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo != nullptr)
        {
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        }
        bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

        HINTERNET session = WinHttpOpen(L"TrafficMonitorAIBalance/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr)
        {
            session = WinHttpOpen(L"TrafficMonitorAIBalance/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session == nullptr)
        {
            result.error = LastErrorText("打开 WinHTTP 会话失败");
            return result;
        }

        WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);

        HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
        if (connection == nullptr)
        {
            result.error = LastErrorText("连接主机失败");
            CloseInternet(session);
            return result;
        }

        HINTERNET request = WinHttpOpenRequest(connection, method, path.c_str(),
            nullptr, referer.empty() ? WINHTTP_NO_REFERER : referer.c_str(),
            WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (request == nullptr)
        {
            result.error = LastErrorText("创建 HTTP 请求失败");
            CloseInternet(connection);
            CloseInternet(session);
            return result;
        }

        WinHttpAddRequestHeaders(request, headers.c_str(),
            static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        DWORD bodyLength = body == nullptr ? 0 : static_cast<DWORD>(body->size());
        BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            body == nullptr || body->empty() ? WINHTTP_NO_REQUEST_DATA
                : const_cast<char*>(&(*body)[0]),
            bodyLength, bodyLength, 0);
        if (!sent || !WinHttpReceiveResponse(request, nullptr))
        {
            result.error = LastErrorText("发送请求失败");
            CloseInternet(request);
            CloseInternet(connection);
            CloseInternet(session);
            return result;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        result.status = static_cast<int>(status);

        while (true)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            {
                break;
            }
            std::string chunk;
            chunk.resize(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0)
            {
                break;
            }
            chunk.resize(read);
            result.body += chunk;
        }

        CloseInternet(request);
        CloseInternet(connection);
        CloseInternet(session);
        return result;
    }

    const wchar_t kUserAgent[] =
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n";
}

HttpResult HttpGet(const std::wstring& url, const std::string& apiKey)
{
    std::wstring headers = L"Accept: application/json\r\n";
    headers += kUserAgent;
    if (!apiKey.empty())
    {
        headers += L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
    }
    return HttpPerform(url, L"GET", nullptr, headers, std::wstring());
}

HttpResult HttpGetWithCookie(const std::wstring& url, const std::string& cookie,
    const std::wstring& origin)
{
    std::wstring headers = L"Accept: application/json\r\n";
    headers += kUserAgent;
    headers += L"Origin: " + origin + L"\r\n";
    if (!cookie.empty())
    {
        headers += L"Cookie: " + Utf8ToWide(cookie) + L"\r\n";
    }
    return HttpPerform(url, L"GET", nullptr, headers, origin + L"/");
}

HttpResult HttpPostForm(const std::wstring& url, const std::string& body,
    const std::string& cookie, const std::wstring& origin)
{
    std::wstring headers = L"Accept: application/json\r\n";
    headers += kUserAgent;
    headers += L"Content-Type: application/x-www-form-urlencoded;charset=UTF-8\r\n";
    headers += L"Origin: " + origin + L"\r\n";
    if (!cookie.empty())
    {
        headers += L"Cookie: " + Utf8ToWide(cookie) + L"\r\n";
    }
    return HttpPerform(url, L"POST", &body, headers, origin + L"/");
}
