#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wincrypt.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#include "PluginInterface.h"
#include "Json.h"
#include "Http.h"
#include "OptionsDialog.h"

namespace
{
    const unsigned long long kDotNetEpochOffset = 504911232000000000ULL;

    struct StateEntry
    {
        double balance;
        unsigned long long ticks;
        double spentToday;
        std::string day;
        StateEntry() : balance(0), ticks(0), spentToday(0) {}
    };

    struct Provider
    {
        std::string id;
        std::string name;
        std::string kind;
        std::string baseUrl;
        std::string key;
        int sortOrder;
        bool hasBalance;
        double balance;
        double spentToday;
        bool spentIsOfficial;
        // Token Plan 返回的是周期用量比例而不是金额；周期由查询结果决定。
        bool weeklyPeriod;
        std::string day;
        unsigned long long lastChangeTicks;
        // 最近一次失败原因：非空时列表里的数值显示“失败”。
        std::string lastError;

        Provider()
            : sortOrder(0), hasBalance(false), balance(0), spentToday(0),
              spentIsOfficial(false), weeklyPeriod(false), lastChangeTicks(0)
        {
        }
    };

    // 任务栏按百分比显示的类型；判定只看类型，不依赖本次查询是否成功，
    // 否则重启后读到旧状态会先按金额格式闪一次。
    bool IsPercentProvider(const Provider& provider)
    {
        return provider.kind == "token_plan" || provider.kind == "xiaomi";
    }

    HMODULE g_module = nullptr;
    ITrafficMonitor* g_app = nullptr;
    std::wstring g_providersPath;
    std::wstring g_balancesPath;
    std::wstring g_statePath;
    std::wstring g_logPath;
    std::wstring g_iniPath;
    DWORD g_refreshSeconds = 60;
    HANDLE g_stopEvent = nullptr;
    HANDLE g_reloadEvent = nullptr;
    HANDLE g_thread = nullptr;
    FILETIME g_providerMtime = {0, 0};
    bool g_hasProviderMtime = false;
    bool g_missingLogged = false;
    std::vector<Provider> g_providers;
    CRITICAL_SECTION g_textLock;
    bool g_textLockInitialized = false;
    std::wstring g_slot1Text;
    std::wstring g_slot2Text;
    CRITICAL_SECTION g_viewLock;
    bool g_viewLockInitialized = false;
    std::vector<vb_options::ProviderValue> g_viewRows;
    DWORD g_lastRefreshTick = 0;
    size_t g_rotationIndex = 0;

    std::wstring WideFromUtf8(const std::string& value)
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

    std::string Utf8FromWide(const std::wstring& value)
    {
        if (value.empty())
        {
            return std::string();
        }
        int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return std::string();
        }
        std::string result;
        result.resize(static_cast<size_t>(length));
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), &result[0], length, nullptr, nullptr);
        return result;
    }

    std::wstring TrimSpaces(const std::wstring& value)
    {
        size_t begin = 0;
        size_t end = value.size();
        while (begin < end && (value[begin] == L' ' || value[begin] == L'\t'))
        {
            ++begin;
        }
        while (end > begin && (value[end - 1] == L' ' || value[end - 1] == L'\t'))
        {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    std::wstring WideFromCodePage(const std::string& value, UINT codePage)
    {
        if (value.empty())
        {
            return std::wstring();
        }
        int length = MultiByteToWideChar(codePage, 0, value.c_str(),
            static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
        {
            return std::wstring();
        }
        std::wstring result;
        result.resize(static_cast<size_t>(length));
        MultiByteToWideChar(codePage, 0, value.c_str(),
            static_cast<int>(value.size()), &result[0], length);
        return result;
    }

    std::wstring DirectoryOf(const std::wstring& path)
    {
        size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return std::wstring();
        }
        return path.substr(0, pos + 1);
    }

    bool FileExists(const std::wstring& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool ReadFileBytes(const std::wstring& path, std::string& data)
    {
        data.clear();
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 8 * 1024 * 1024)
        {
            CloseHandle(file);
            return false;
        }
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!data.empty()
            && (!ReadFile(file, &data[0], static_cast<DWORD>(data.size()), &read, nullptr)
                || read != data.size()))
        {
            CloseHandle(file);
            data.clear();
            return false;
        }
        CloseHandle(file);
        if (data.size() >= 3
            && static_cast<unsigned char>(data[0]) == 0xEF
            && static_cast<unsigned char>(data[1]) == 0xBB
            && static_cast<unsigned char>(data[2]) == 0xBF)
        {
            data.erase(0, 3);
        }
        return true;
    }

    bool WriteFileAtomic(const std::wstring& path, const std::string& data)
    {
        std::wstring temp = path + L".tmp";
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written = 0;
        bool ok = data.empty()
            || (WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
                && written == data.size());
        CloseHandle(file);
        if (!ok)
        {
            DeleteFileW(temp.c_str());
            return false;
        }
        if (!MoveFileExW(temp.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }

    std::wstring GetTimestamp()
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t buffer[64];
        swprintf(buffer, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::wstring(buffer);
    }

    void Log(const std::wstring& message)
    {
        if (g_logPath.empty())
        {
            return;
        }
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &attributes)
            && attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow > 1024 * 1024)
        {
            std::wstring oldLog = g_logPath + L".old";
            DeleteFileW(oldLog.c_str());
            MoveFileExW(g_logPath.c_str(), oldLog.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        std::wstring line = GetTimestamp() + L"  " + message + L"\r\n";
        int length = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
            static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return;
        }
        std::string bytes;
        bytes.resize(static_cast<size_t>(length));
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
            static_cast<int>(line.size()), &bytes[0], length, nullptr, nullptr);
        HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(file);
    }

    unsigned long long CurrentDotNetTicks()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER value;
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return value.QuadPart + kDotNetEpochOffset;
    }

    long long CurrentUnixSeconds()
    {
        return static_cast<long long>(time(nullptr));
    }

    std::string AmountText2(double value)
    {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.2f", value);
        return std::string(buffer);
    }

    std::string PercentText1(double value)
    {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.1f", value);
        return std::string(buffer);
    }

    void FormatAmountPair(double balance, double spent,
        std::wstring& balanceField, std::wstring& spentField)
    {
        std::string balanceText = AmountText2(balance);
        std::string spentText = AmountText2(spent);
        size_t balanceDot = balanceText.find('.');
        size_t spentDot = spentText.find('.');
        int balanceInteger = static_cast<int>(
            balanceDot == std::string::npos ? balanceText.size() : balanceDot);
        int spentInteger = static_cast<int>(
            spentDot == std::string::npos ? spentText.size() : spentDot);
        int width = balanceInteger > spentInteger ? balanceInteger : spentInteger;
        if (balanceInteger < width)
        {
            balanceText.insert(0, static_cast<size_t>((width - balanceInteger) * 2), ' ');
        }
        if (spentInteger < width)
        {
            spentText.insert(0, static_cast<size_t>((width - spentInteger) * 2), ' ');
        }
        balanceField = WideFromUtf8(balanceText);
        spentField = WideFromUtf8(spentText);
    }


    std::string LocalDayString()
    {
        char buffer[16] = "";
        time_t now = time(nullptr);
        struct tm* tmValue = localtime(&now);
        if (tmValue != nullptr)
        {
            strftime(buffer, sizeof(buffer), "%Y-%m-%d", tmValue);
        }
        return std::string(buffer);
    }

    int DayNumber(const std::string& day)
    {
        int year = 0;
        int month = 0;
        int date = 0;
        if (sscanf(day.c_str(), "%d-%d-%d", &year, &month, &date) != 3)
        {
            return -1;
        }
        struct tm value;
        ZeroMemory(&value, sizeof(value));
        value.tm_year = year - 1900;
        value.tm_mon = month - 1;
        value.tm_mday = date;
        value.tm_hour = 12;
        value.tm_isdst = -1;
        time_t seconds = mktime(&value);
        if (seconds == static_cast<time_t>(-1))
        {
            return -1;
        }
        return static_cast<int>(seconds / 86400);
    }

    bool IsWideChar(wchar_t ch)
    {
        unsigned int code = static_cast<unsigned int>(ch);
        return (code >= 0x1100 && code <= 0x115F)
            || (code >= 0x2E80 && code <= 0xA4CF)
            || (code >= 0xAC00 && code <= 0xD7A3)
            || (code >= 0xF900 && code <= 0xFAFF)
            || (code >= 0xFE30 && code <= 0xFE4F)
            || (code >= 0xFF00 && code <= 0xFF60)
            || (code >= 0xFFE0 && code <= 0xFFE6);
    }

    std::string NumberRaw(double value)
    {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.17g", value);
        return std::string(buffer);
    }

    std::string AmountText4(double value)
    {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.4f", value);
        return std::string(buffer);
    }

    void SetSlotTexts(const std::wstring& first, const std::wstring& second)
    {
        if (!g_textLockInitialized)
        {
            return;
        }
        EnterCriticalSection(&g_textLock);
        g_slot1Text = first;
        g_slot2Text = second;
        LeaveCriticalSection(&g_textLock);
    }

    // 解析 balances.txt：取出两行显示文本与最后写入时刻（epoch 秒）。
    bool ParseBalancesText(const std::wstring& wide, std::wstring& first,
        std::wstring& second, long long& updated)
    {
        bool hasFirst = false;
        size_t position = 0;
        while (position <= wide.size())
        {
            size_t end = wide.find(L'\n', position);
            if (end == std::wstring::npos)
            {
                end = wide.size();
            }
            std::wstring line = wide.substr(position, end - position);
            if (!line.empty() && line[line.size() - 1] == L'\r')
            {
                line.erase(line.size() - 1);
            }

            static const std::wstring firstPrefix = L"SLOT1: ";
            static const std::wstring secondPrefix = L"SLOT2: ";
            if (line.compare(0, firstPrefix.size(), firstPrefix) == 0)
            {
                first = line.substr(firstPrefix.size());
                hasFirst = true;
            }
            else if (line.compare(0, secondPrefix.size(), secondPrefix) == 0)
            {
                second = line.substr(secondPrefix.size());
            }
            else if (line.compare(0, 8, L"updated=") == 0)
            {
                updated = wcstoll(line.c_str() + 8, nullptr, 10);
            }

            if (end == wide.size())
            {
                break;
            }
            position = end + 1;
        }
        return hasFirst;
    }

    // 启动时只恢复 15 分钟内的 balances.txt 缓存；过期则留空，让显示项立即返回 --。
    bool ApplyCachedBalances(const std::string& bytes, long long now)
    {
        if (bytes.empty())
        {
            return false;
        }
        std::wstring wide = WideFromCodePage(bytes, 936);
        if (wide.empty())
        {
            return false;
        }

        std::wstring first;
        std::wstring second;
        long long updated = 0;
        if (!ParseBalancesText(wide, first, second, updated))
        {
            return false;
        }

        DWORD ageMilliseconds = 0;
        if (updated > 0)
        {
            long long ageSeconds = now - updated;
            if (ageSeconds > 900)
            {
                return false;
            }
            if (ageSeconds < 0)
            {
                ageSeconds = 0;
            }
            ageMilliseconds = static_cast<DWORD>(ageSeconds * 1000);
        }

        SetSlotTexts(first, second);
        g_lastRefreshTick = GetTickCount() - ageMilliseconds;
        return true;
    }

    void LoadCachedBalances()
    {
        std::string bytes;
        if (ReadFileBytes(g_balancesPath, bytes))
        {
            ApplyCachedBalances(bytes, CurrentUnixSeconds());
        }
    }

    std::string ClipBody(const std::string& body, size_t max)
    {
        std::string result = body;
        for (size_t i = 0; i < result.size(); ++i)
        {
            if (result[i] == '\r' || result[i] == '\n')
            {
                result[i] = ' ';
            }
        }
        if (result.size() > max)
        {
            result = result.substr(0, max) + "...";
        }
        return result;
    }

    bool UnprotectKey(const std::string& base64, std::string& key)
    {
        key.clear();
        if (base64.empty())
        {
            return false;
        }
        DWORD binaryLength = 0;
        if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()),
            CRYPT_STRING_BASE64, nullptr, &binaryLength, nullptr, nullptr)
            || binaryLength == 0)
        {
            return false;
        }
        std::vector<BYTE> binary(binaryLength);
        if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()),
            CRYPT_STRING_BASE64, binary.data(), &binaryLength, nullptr, nullptr))
        {
            return false;
        }
        DATA_BLOB input;
        input.pbData = binary.data();
        input.cbData = binaryLength;
        DATA_BLOB output;
        ZeroMemory(&output, sizeof(output));
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output))
        {
            return false;
        }
        key.assign(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
        return true;
    }

    // DPAPI 当前用户作用域加密，与旧版 C# 管理器的 ProtectedData 兼容。
    // 空密钥不加密，直接存空串。
    bool ProtectKey(const std::string& key, std::string& base64)
    {
        base64.clear();
        if (key.empty())
        {
            return true;
        }
        DATA_BLOB input;
        input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(key.data()));
        input.cbData = static_cast<DWORD>(key.size());
        DATA_BLOB output;
        ZeroMemory(&output, sizeof(output));
        if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output))
        {
            return false;
        }
        DWORD characters = 0;
        DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
        BOOL sized = CryptBinaryToStringA(output.pbData, output.cbData, flags, nullptr, &characters);
        std::string text;
        if (sized && characters > 1)
        {
            text.resize(static_cast<size_t>(characters));
            if (CryptBinaryToStringA(output.pbData, output.cbData, flags, &text[0], &characters))
            {
                text.resize(::strnlen(text.c_str(), text.size()));
            }
            else
            {
                text.clear();
            }
        }
        LocalFree(output.pbData);
        if (text.empty())
        {
            return false;
        }
        base64 = text;
        return true;
    }

    std::string JsonEscape(const std::string& text)
    {
        std::string result;
        for (size_t i = 0; i < text.size(); ++i)
        {
            char ch = text[i];
            switch (ch)
            {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    char buffer[8];
                    snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                    result += buffer;
                }
                else
                {
                    result += ch;
                }
                break;
            }
        }
        return result;
    }

    std::string JsonString(const JsonValue& object, const char* key)
    {
        const JsonValue* value = object.Get(key);
        return value == nullptr ? std::string() : value->AsString(std::string());
    }

    bool JsonNumber(const JsonValue* object, const char* key, double& value)
    {
        if (object == nullptr)
        {
            return false;
        }
        const JsonValue* field = object->Get(key);
        if (field == nullptr)
        {
            return false;
        }
        if (field->IsNumber())
        {
            value = field->numberValue;
            return true;
        }
        if (field->IsString())
        {
            char* end = nullptr;
            double parsed = strtod(field->stringValue.c_str(), &end);
            if (end != field->stringValue.c_str())
            {
                value = parsed;
                return true;
            }
        }
        return false;
    }

    std::wstring TrimTrailingSlash(std::wstring value)
    {
        while (!value.empty() && (value[value.size() - 1] == L'/' || value[value.size() - 1] == L'\\'))
        {
            value.erase(value.size() - 1);
        }
        return value;
    }

    bool EndsWithW(const std::wstring& value, const wchar_t* suffix)
    {
        size_t length = wcslen(suffix);
        if (value.size() < length)
        {
            return false;
        }
        return _wcsicmp(value.c_str() + value.size() - length, suffix) == 0;
    }

    std::wstring BuildEndpoint(std::wstring base, const wchar_t* path)
    {
        base = TrimTrailingSlash(base);
        if (EndsWithW(base, path))
        {
            return base;
        }
        const wchar_t* prefixes[] = { L"/openapi/v1", L"/api/v1", L"/v1" };
        for (int i = 0; i < 3; ++i)
        {
            std::wstring prefix = prefixes[i];
            if (EndsWithW(base, prefix.c_str())
                && _wcsnicmp(path, (prefix + L"/").c_str(), prefix.size() + 1) == 0)
            {
                base = base.substr(0, base.size() - prefix.size());
                break;
            }
        }
        return base + path;
    }

    std::wstring ProviderBase(const Provider& provider, const wchar_t* fallback)
    {
        std::wstring base = provider.baseUrl.empty()
            ? std::wstring(fallback)
            : WideFromUtf8(provider.baseUrl);
        return TrimTrailingSlash(base);
    }

    bool EnsureHttpOk(const HttpResult& result, const std::string& labelUtf8, std::string& error)
    {
        if (!result.error.empty())
        {
            error = result.error;
            return false;
        }
        if (result.status < 200 || result.status >= 300)
        {
            // 对话框状态行只有一行（344 DIP ≈ 24 个汉字），响应体写日志，
            // 界面只给“哪一步失败 + 看日志”的短结论。
            Log(L"HTTP " + std::to_wstring(result.status) + L" "
                + WideFromUtf8(labelUtf8) + L"：" + WideFromUtf8(ClipBody(result.body, 400)));
            // labelUtf8 与 error 都按 UTF-8 传递；这里不能先转 ANSI，
            // 否则中文供应商名会在日志和测试结果里变成乱码。
            error = labelUtf8 + " HTTP " + std::to_string(result.status)
                + "，详见日志";
            return false;
        }
        return true;
    }

    bool ParseSub2Api(const std::string& body, double& balance,
        double& spentToday, bool& hasSpent, std::string& error)
    {
        JsonValue root;
        if (!ParseJson(body, root, error) || !root.IsObject())
        {
            error = "响应不是 JSON 对象";
            return false;
        }

        bool foundBalance = JsonNumber(&root, "balance", balance)
            || JsonNumber(&root, "remaining", balance);
        if (!foundBalance)
        {
            const JsonValue* data = root.Get("data");
            if (data != nullptr)
            {
                foundBalance = JsonNumber(data, "balance", balance)
                    || JsonNumber(data, "remaining", balance);
            }
        }
        if (!foundBalance)
        {
            const JsonValue* quota = root.Get("quota");
            if (quota != nullptr)
            {
                foundBalance = JsonNumber(quota, "remaining", balance)
                    || JsonNumber(quota, "balance", balance);
            }
        }
        if (!foundBalance)
        {
            error = "响应中缺少 balance/remaining";
            return false;
        }

        hasSpent = false;
        const JsonValue* usage = root.Get("usage");
        if (usage != nullptr && usage->IsObject())
        {
            const JsonValue* today = usage->Get("today");
            if (today != nullptr && today->IsObject())
            {
                hasSpent = JsonNumber(today, "actual_cost", spentToday)
                    || JsonNumber(today, "actualCost", spentToday);
            }
        }

        if (!hasSpent)
        {
            const JsonValue* daily = root.Get("daily_usage");
            if (daily != nullptr && daily->IsArray())
            {
                const std::string todayDay = LocalDayString();
                for (size_t i = 0; i < daily->Size(); ++i)
                {
                    const JsonValue* item = daily->At(i);
                    if (item == nullptr || !item->IsObject())
                    {
                        continue;
                    }
                    if (JsonString(*item, "date") != todayDay)
                    {
                        continue;
                    }
                    if (JsonNumber(item, "actual_cost", spentToday)
                        || JsonNumber(item, "actualCost", spentToday)
                        || JsonNumber(item, "cost", spentToday))
                    {
                        hasSpent = true;
                        break;
                    }
                }
            }
        }
        return true;
    }

    bool ParseDeepSeek(const std::string& body, double& balance, std::string& error)
    {
        JsonValue root;
        if (!ParseJson(body, root, error) || !root.IsObject())
        {
            error = "响应不是 JSON 对象";
            return false;
        }
        const JsonValue* available = root.Get("is_available");
        if (available == nullptr)
        {
            available = root.Get("isAvailable");
        }
        if (available != nullptr && !available->AsBool(true))
        {
            error = "账户不可用";
            return false;
        }
        const JsonValue* infos = root.Get("balance_infos");
        if (infos == nullptr)
        {
            infos = root.Get("balanceInfos");
        }
        if (infos == nullptr || !infos->IsArray() || infos->Size() == 0)
        {
            error = "响应缺少 balance_infos";
            return false;
        }
        bool found = false;
        bool foundCny = false;
        for (size_t i = 0; i < infos->Size(); ++i)
        {
            const JsonValue* info = infos->At(i);
            if (info == nullptr || !info->IsObject())
            {
                continue;
            }
            double value = 0;
            if (!JsonNumber(info, "total_balance", value)
                && !JsonNumber(info, "totalBalance", value))
            {
                continue;
            }
            std::string currency = JsonString(*info, "currency");
            if (!found || (!foundCny && currency == "CNY" && value > 0))
            {
                balance = value;
                found = true;
                foundCny = currency == "CNY";
            }
        }
        if (!found)
        {
            error = "响应缺少余额字段";
            return false;
        }
        return true;
    }

    bool ParseMoonshot(const std::string& body, double& balance, std::string& error)
    {
        JsonValue root;
        if (!ParseJson(body, root, error) || !root.IsObject())
        {
            error = "响应不是 JSON 对象";
            return false;
        }
        double code = 0;
        if (root.Get("code") != nullptr && JsonNumber(&root, "code", code) && code != 0)
        {
            error = "接口返回 code " + NumberRaw(code);
            return false;
        }
        const JsonValue* data = root.Get("data");
        if (data == nullptr || !data->IsObject())
        {
            error = "响应缺少 data";
            return false;
        }
        if (JsonNumber(data, "available_balance", balance)
            || JsonNumber(data, "availableBalance", balance)
            || JsonNumber(data, "availablebalance", balance)
            || JsonNumber(data, "balance", balance))
        {
            return true;
        }
        error = "响应缺少 data.available_balance";
        return false;
    }

    bool ParseOneApi(const std::string& subscriptionBody, const std::string& usageBody,
        double& balance, std::string& error)
    {
        JsonValue subscription;
        if (!ParseJson(subscriptionBody, subscription, error) || !subscription.IsObject())
        {
            error = "订阅响应不是 JSON 对象";
            return false;
        }
        double hard = 0;
        if (!JsonNumber(&subscription, "hard_limit_usd", hard)
            && !JsonNumber(&subscription, "hardLimitUsd", hard)
            && !JsonNumber(&subscription, "system_hard_limit_usd", hard)
            && !JsonNumber(&subscription, "systemHardLimitUsd", hard)
            && !JsonNumber(&subscription, "soft_limit_usd", hard))
        {
            error = "响应缺少 hard_limit_usd";
            return false;
        }
        if (hard >= 100000000)
        {
            error = "是不限额 Key，无法计算余额";
            return false;
        }
        JsonValue usage;
        if (!ParseJson(usageBody, usage, error) || !usage.IsObject())
        {
            error = "用量响应不是 JSON 对象";
            return false;
        }
        double totalUsage = 0;
        if (!JsonNumber(&usage, "total_usage", totalUsage)
            && !JsonNumber(&usage, "totalUsage", totalUsage))
        {
            error = "响应缺少 total_usage";
            return false;
        }
        balance = hard - totalUsage / 100.0;
        return true;
    }

    bool ParseOneApiUsageTotal(const std::string& body, double& units, std::string& error)
    {
        JsonValue usage;
        if (!ParseJson(body, usage, error) || !usage.IsObject())
        {
            error = "用量响应不是 JSON 对象";
            return false;
        }
        if (!JsonNumber(&usage, "total_usage", units)
            && !JsonNumber(&usage, "totalUsage", units))
        {
            error = "响应缺少 total_usage";
            return false;
        }
        return true;
    }

    // ---- 千问 Token Plan 个人版：控制台接口只认浏览器登录 Cookie ----
    //
    // 取证来源（2026-10-20 实测 platform.qianwenai.com 个人版用量页）：
    //   1) GET  https://platform-home.qianwenai.com/tool/user/info.json
    //      -> data.secToken（网关要求的表单字段），data.sessionExpireTimeStamp
    //   2) POST https://cs-data.qianwenai.com/data/api.json
    //           ?product=sfm_bailian&action=BroadScopeAspnGateway&api=<Api>
    //      表单 product/action/sec_token/region/params，params 里必须带
    //      V=1.0 和 Data.cornerstoneParam，否则网关回 Bad Request。
    //   用量字段 data.DataV2.data.data.per1WeekPercentage / per1MonthPercentage
    //   是 0-1 的比例；个人版 lite 档只有月度字段。
    // 控制台 Cookie 失效时的统一文案：状态行只有一行，太长会被省略。
    const char kCookieExpiredError[] = "Cookie 已失效，请重新登录后复制";

    const char kTokenPlanHome[] = "https://platform-home.qianwenai.com";
    const char kTokenPlanGateway[] = "https://cs-data.qianwenai.com";
    const char kTokenPlanOrigin[] = "https://platform.qianwenai.com";
    const char kTokenPlanApi[] =
        "zeldaHttp.apikeyMgr./tokenplan/personal/api/v2/usage";
    // 网关要求 params 里同时带 Api、cornerstoneParam 和 V=1.0，缺一个就 Bad Request。
    std::string TokenPlanParams()
    {
        std::string params = "{\"Api\":\"";
        params += kTokenPlanApi;
        params += "\",\"Data\":{\"cornerstoneParam\":{\"domain\":\"platform.qianwenai.com\","
            "\"consoleSite\":\"QIANWENAI\",\"console\":\"ONE_CONSOLE\","
            "\"xsp_lang\":\"zh-CN\",\"protocol\":\"V2\",\"productCode\":\"p_efm\"}},"
            "\"V\":\"1.0\"}";
        return params;
    }

    std::string UrlEncode(const std::string& text)
    {
        static const char kHex[] = "0123456789ABCDEF";
        std::string out;
        for (size_t i = 0; i < text.size(); ++i)
        {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
                || ch == '.' || ch == '~';
            if (safe)
            {
                out.push_back(static_cast<char>(ch));
            }
            else
            {
                out.push_back('%');
                out.push_back(kHex[ch >> 4]);
                out.push_back(kHex[ch & 15]);
            }
        }
        return out;
    }

    bool ParseSecToken(const std::string& body, std::string& secToken,
        double& expireStamp, std::string& error)
    {
        JsonValue root;
        if (!ParseJson(body, root, error) || !root.IsObject())
        {
            error = "登录信息响应不是 JSON，Cookie 可能已失效";
            return false;
        }
        const JsonValue* data = root.Get("data");
        if (data == nullptr || !data->IsObject())
        {
            // 未登录时接口返回 {"code":"ConsoleNeedLogin","message":"请登录"}，
            // 没有 data，这里把原因写清楚，免得日志只说“解析失败”。
            std::string code = JsonString(root, "code");
            std::string message = JsonString(root, "message");
            if (code == "ConsoleNeedLogin")
            {
                error = kCookieExpiredError;
            }
            else
            {
                error = "登录信息响应缺少 data" + (message.empty()
                    ? std::string("：" + ClipBody(body, 160))
                    : std::string("：") + message);
            }
            return false;
        }
        secToken = JsonString(*data, "secToken");
        if (secToken.empty())
        {
            error = kCookieExpiredError;
            return false;
        }
        JsonNumber(data, "sessionExpireTimeStamp", expireStamp);
        return true;
    }

    bool ParseTokenPlanUsage(const std::string& body, double& percentage,
        bool& weekly, std::string& error)
    {
        JsonValue root;
        if (!ParseJson(body, root, error) || !root.IsObject())
        {
            error = "用量响应不是 JSON 对象";
            return false;
        }
        const JsonValue* data = root.Get("data");
        const JsonValue* dataV2 = data == nullptr ? nullptr : data->Get("DataV2");
        const JsonValue* inner = dataV2 == nullptr ? nullptr : dataV2->Get("data");
        const JsonValue* usage = inner == nullptr ? nullptr : inner->Get("data");
        if (usage == nullptr || !usage->IsObject())
        {
            std::string message = data == nullptr ? std::string()
                : JsonString(*data, "errorMsg");
            if (message.find("NotLogined") != std::string::npos)
            {
                error = kCookieExpiredError;
            }
            else
            {
                // 网关原文可能很长，写日志；界面只给短结论。
                Log(L"Token Plan 网关响应：" + WideFromUtf8(ClipBody(body, 400)));
                error = message.empty() ? "用量响应缺少字段，详见日志"
                    : "网关返回错误，详见日志";
            }
            return false;
        }
        if (JsonNumber(usage, "per1WeekPercentage", percentage))
        {
            weekly = true;
        }
        else if (JsonNumber(usage, "per1MonthPercentage", percentage))
        {
            weekly = false;
        }
        else
        {
            Log(L"Token Plan 用量响应：" + WideFromUtf8(ClipBody(body, 400)));
            error = "用量响应缺少百分比，详见日志";
            return false;
        }
        if (!(percentage >= 0.0 && percentage <= 1.0))
        {
            error = "用量比例超出 0-1：" + NumberRaw(percentage);
            return false;
        }
        return true;
    }

    // 比例型用量（0-1）统一折算成“剩余/已用百分比”，并标记为官方口径。
    void ApplyPercentUsage(Provider& provider, double percentage, bool weekly)
    {
        provider.weeklyPeriod = weekly;
        provider.balance = (1.0 - percentage) * 100.0;
        provider.spentToday = percentage * 100.0;
        provider.spentIsOfficial = true;
    }

    bool QueryTokenPlan(Provider& provider, std::string& error)
    {
        std::wstring origin = WideFromUtf8(kTokenPlanOrigin);

        HttpResult info = HttpGetWithCookie(
            WideFromUtf8(kTokenPlanHome) + L"/tool/user/info.json",
            provider.key, origin);
        if (!EnsureHttpOk(info, provider.name, error))
        {
            return false;
        }
        std::string secToken;
        double expireStamp = 0;
        if (!ParseSecToken(info.body, secToken, expireStamp, error))
        {
            return false;
        }

        std::string body;
        body += "product=sfm_bailian&action=BroadScopeAspnGateway&sec_token=";
        body += UrlEncode(secToken);
        body += "&region=cn-beijing&params=";
        body += UrlEncode(TokenPlanParams());

        std::wstring url = WideFromUtf8(kTokenPlanGateway)
            + L"/data/api.json?product=sfm_bailian&action=BroadScopeAspnGateway&api="
            + WideFromUtf8(UrlEncode(kTokenPlanApi));
        HttpResult usage = HttpPostForm(url, body, provider.key, origin);
        if (!EnsureHttpOk(usage, provider.name, error))
        {
            return false;
        }
        double percentage = 0;
        bool weekly = false;
        if (!ParseTokenPlanUsage(usage.body, percentage, weekly, error))
        {
            return false;
        }
        ApplyPercentUsage(provider, percentage, weekly);
        if (expireStamp > 0)
        {
            // 把会话到期时间写进日志：Cookie 失效前用户能看到还剩多久。
            time_t expireTime = static_cast<time_t>(expireStamp / 1000.0);
            struct tm* expireTm = localtime(&expireTime);
            if (expireTm != nullptr)
            {
                wchar_t text[96];
                swprintf(text, 96,
                    L"Token Plan 登录会话到期 %4d-%02d-%02d %02d:%02d",
                    expireTm->tm_year + 1900, expireTm->tm_mon + 1,
                    expireTm->tm_mday, expireTm->tm_hour, expireTm->tm_min);
                Log(text);
            }
        }
        return true;
    }

    // ---- 小米 MiMo 开放平台：TokenPlan 用量同样只认控制台 Cookie ----
    //
    // 实测（2026-10-21）：
    //   GET https://platform.xiaomimimo.com/api/v1/tokenPlan/usage
    //   -> data.monthUsage.percent（0-1）、data.monthUsage.items[0]
    //      { name:"month_total_token", used, limit, percent }
    //   未登录返回 HTTP 401 + {"code":401,"loginUrl":...}。
    const char kXiaomiBase[] = "https://platform.xiaomimimo.com";

    bool ParseXiaomiUsage(const std::string& body, double& percentage, std::string& error)
    {
        JsonValue root;
        if (!ParseJson(body, root, error) || !root.IsObject())
        {
            error = "用量响应不是 JSON 对象";
            return false;
        }
        double code = 0;
        if (JsonNumber(&root, "code", code) && code != 0)
        {
            Log(L"小米网关响应：" + WideFromUtf8(ClipBody(body, 300)));
            error = "小米返回错误，详见日志";
            return false;
        }
        const JsonValue* data = root.Get("data");
        const JsonValue* month = data == nullptr ? nullptr : data->Get("monthUsage");
        const JsonValue* usage = data == nullptr ? nullptr : data->Get("usage");
        bool found = month != nullptr && JsonNumber(month, "percent", percentage);
        if (!found)
        {
            found = usage != nullptr && JsonNumber(usage, "percent", percentage);
        }
        if (!found)
        {
            Log(L"小米用量响应：" + WideFromUtf8(ClipBody(body, 300)));
            error = "用量响应缺少百分比，详见日志";
            return false;
        }
        if (!(percentage >= 0.0 && percentage <= 1.0))
        {
            error = "用量比例超出 0-1：" + NumberRaw(percentage);
            return false;
        }
        return true;
    }

    bool QueryXiaomi(Provider& provider, std::string& error)
    {
        std::wstring origin = WideFromUtf8(kXiaomiBase);
        HttpResult result = HttpGetWithCookie(
            WideFromUtf8(kXiaomiBase) + L"/api/v1/tokenPlan/usage",
            provider.key, origin);
        if (result.status == 401)
        {
            // 未登录时返回 401 + loginUrl，直接给结论，不把重定向串塞进界面。
            error = kCookieExpiredError;
            return false;
        }
        if (!EnsureHttpOk(result, provider.name, error))
        {
            return false;
        }
        double percentage = 0;
        if (!ParseXiaomiUsage(result.body, percentage, error))
        {
            return false;
        }
        ApplyPercentUsage(provider, percentage, false);
        return true;
    }

    bool QueryProvider(Provider& provider, std::string& error)
    {
        error.clear();
        if (provider.key.empty())
        {
            error = "未配置 API Key";
            return false;
        }
        std::string label = provider.name;
        std::wstring base;
        HttpResult result;

        if (provider.kind == "token_plan")
        {
            return QueryTokenPlan(provider, error);
        }

        if (provider.kind == "xiaomi")
        {
            return QueryXiaomi(provider, error);
        }

        if (provider.kind == "deepseek")
        {
            base = ProviderBase(provider, L"https://api.deepseek.com");
            if (EndsWithW(base, L"/v1"))
            {
                base = base.substr(0, base.size() - 3);
            }
            std::wstring url = EndsWithW(base, L"/user/balance")
                ? base
                : base + L"/user/balance";
            result = HttpGet(url, provider.key);
            if (!EnsureHttpOk(result, label, error))
            {
                return false;
            }
            bool ok = ParseDeepSeek(result.body, provider.balance, error);
            if (ok)
            {
                provider.spentIsOfficial = false;
            }
            return ok;
        }

        if (provider.kind == "moonshot")
        {
            base = ProviderBase(provider, L"https://api.moonshot.cn");
            std::wstring url = BuildEndpoint(base, L"/v1/users/me/balance");
            result = HttpGet(url, provider.key);
            if (!EnsureHttpOk(result, label, error))
            {
                return false;
            }
            bool ok = ParseMoonshot(result.body, provider.balance, error);
            if (ok)
            {
                provider.spentIsOfficial = false;
            }
            return ok;
        }

        if (provider.kind == "oneapi")
        {
            base = ProviderBase(provider, L"");
            if (base.empty())
            {
                error = "请填写 One-API 站点地址";
                return false;
            }
            std::wstring subscriptionUrl = BuildEndpoint(base,
                L"/v1/dashboard/billing/subscription");
            result = HttpGet(subscriptionUrl, provider.key);
            if (!EnsureHttpOk(result, label, error))
            {
                return false;
            }
            std::string subscriptionBody = result.body;

            time_t tomorrow = time(nullptr) + 86400;
            struct tm* tmValue = localtime(&tomorrow);
            char endDate[32] = "2099-12-31";
            if (tmValue != nullptr)
            {
                strftime(endDate, sizeof(endDate), "%Y-%m-%d", tmValue);
            }
            std::wstring usageEndpoint = BuildEndpoint(base,
                L"/v1/dashboard/billing/usage");
            std::wstring usageUrl = usageEndpoint
                + L"?start_date=2020-01-01&end_date=" + WideFromUtf8(endDate);
            result = HttpGet(usageUrl, provider.key);
            if (!EnsureHttpOk(result, label, error))
            {
                return false;
            }
            if (!ParseOneApi(subscriptionBody, result.body, provider.balance, error))
            {
                return false;
            }

            std::wstring todayUsageUrl = usageEndpoint
                + L"?start_date=" + WideFromUtf8(LocalDayString())
                + L"&end_date=" + WideFromUtf8(endDate);
            HttpResult todayResult = HttpGet(todayUsageUrl, provider.key);
            provider.spentIsOfficial = false;
            if (todayResult.error.empty()
                && todayResult.status >= 200 && todayResult.status < 300)
            {
                double todayUnits = 0;
                std::string todayError;
                // 同上：今日 total_usage 为 0 时不当数，退回余额差累计。
                if (ParseOneApiUsageTotal(todayResult.body, todayUnits, todayError)
                    && todayUnits > 0.0)
                {
                    provider.spentToday = todayUnits / 100.0;
                    provider.spentIsOfficial = true;
                }
            }
            return true;
        }

        base = ProviderBase(provider, L"");
        if (base.empty())
        {
            error = "请填写 Sub2API 站点地址";
            return false;
        }
        if (EndsWithW(base, L"/v1"))
        {
            base = base.substr(0, base.size() - 3);
        }
        std::wstring url = base + L"/v1/usage";
        result = HttpGet(url, provider.key);
        if (!EnsureHttpOk(result, label, error))
        {
            return false;
        }
        double officialSpent = 0;
        bool hasOfficialSpent = false;
        if (!ParseSub2Api(result.body, provider.balance, officialSpent,
                hasOfficialSpent, error))
        {
            return false;
        }
        // 官方今日值是 0 时不当数：Sub2API 的 usage 按 key 统计，同一账户下别的
        // key 在花钱时这把 key 会一直报 0，而余额是账户级的，此时退回余额差累计。
        provider.spentIsOfficial = hasOfficialSpent && officialSpent > 0.0;
        if (provider.spentIsOfficial)
        {
            provider.spentToday = officialSpent;
        }
        return true;
    }

    void LoadStateMap(std::map<std::string, StateEntry>& states)
    {
        states.clear();
        std::string data;
        if (!ReadFileBytes(g_statePath, data))
        {
            return;
        }
        size_t start = 0;
        while (start < data.size())
        {
            size_t end = data.find('\n', start);
            if (end == std::string::npos)
            {
                end = data.size();
            }
            std::string line = data.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line[line.size() - 1] == '\r')
            {
                line.erase(line.size() - 1);
            }
            size_t first = line.find('|');
            size_t second = first == std::string::npos ? std::string::npos : line.find('|', first + 1);
            if (first == std::string::npos || second == std::string::npos)
            {
                continue;
            }
            size_t third = line.find('|', second + 1);
            std::string id = line.substr(0, first);
            std::string balanceText = line.substr(first + 1, second - first - 1);
            std::string ticksText = third == std::string::npos
                ? line.substr(second + 1)
                : line.substr(second + 1, third - second - 1);
            char* endPtr = nullptr;
            double balance = strtod(balanceText.c_str(), &endPtr);
            if (endPtr == balanceText.c_str())
            {
                continue;
            }
            unsigned long long ticks = strtoull(ticksText.c_str(), nullptr, 10);

            StateEntry entry;
            entry.balance = balance;
            entry.ticks = ticks;
            if (third != std::string::npos)
            {
                size_t fourth = line.find('|', third + 1);
                entry.day = fourth == std::string::npos
                    ? line.substr(third + 1)
                    : line.substr(third + 1, fourth - third - 1);
                if (fourth != std::string::npos)
                {
                    std::string spentText = line.substr(fourth + 1);
                    entry.spentToday = strtod(spentText.c_str(), nullptr);
                }
            }
            states[id] = entry;
        }
    }

    bool LoadProviders()
    {
        std::string body;
        if (!ReadFileBytes(g_providersPath, body))
        {
            g_providers.clear();
            if (!g_missingLogged)
            {
                Log(L"providers.json 不存在：" + g_providersPath);
                g_missingLogged = true;
            }
            return false;
        }
        g_missingLogged = false;

        JsonValue root;
        std::string error;
        if (!ParseJson(body, root, error) || !root.IsArray())
        {
            Log(L"providers.json 解析失败：" + WideFromUtf8(error));
            return false;
        }

        std::map<std::string, StateEntry> states;
        LoadStateMap(states);
        std::vector<Provider> loaded;
        for (size_t i = 0; i < root.Size(); ++i)
        {
            const JsonValue* item = root.At(i);
            if (item == nullptr || !item->IsObject())
            {
                continue;
            }
            Provider provider;
            provider.id = JsonString(*item, "Id");
            provider.name = JsonString(*item, "Name");
            provider.kind = JsonString(*item, "Kind");
            provider.baseUrl = JsonString(*item, "BaseUrl");
            std::string protectedKey = JsonString(*item, "KeyProtected");
            double sortOrder = static_cast<double>(i);
            JsonNumber(item, "SortOrder", sortOrder);
            provider.sortOrder = static_cast<int>(sortOrder);
            if (provider.id.empty())
            {
                continue;
            }
            if (provider.name.empty())
            {
                provider.name = provider.id;
            }
            if (provider.kind.empty())
            {
                provider.kind = "sub2api";
            }
            if (!protectedKey.empty())
            {
                if (!UnprotectKey(protectedKey, provider.key))
                {
                    Log(L"Key 解密失败：" + WideFromUtf8(provider.name));
                }
            }

            for (size_t j = 0; j < g_providers.size(); ++j)
            {
                if (g_providers[j].id == provider.id)
                {
                    provider.hasBalance = g_providers[j].hasBalance;
                    provider.balance = g_providers[j].balance;
                    provider.spentToday = g_providers[j].spentToday;
                    provider.spentIsOfficial = g_providers[j].spentIsOfficial;
                    provider.day = g_providers[j].day;
                    provider.lastChangeTicks = g_providers[j].lastChangeTicks;
                    break;
                }
            }
            if (!provider.hasBalance)
            {
                std::map<std::string, StateEntry>::iterator state = states.find(provider.id);
                if (state != states.end())
                {
                    provider.hasBalance = true;
                    provider.balance = state->second.balance;
                    provider.spentToday = state->second.spentToday;
                    provider.day = state->second.day;
                    provider.lastChangeTicks = state->second.ticks;
                }
            }
            loaded.push_back(provider);
        }
        g_providers.swap(loaded);
        Log(L"已加载供应商：" + std::to_wstring(g_providers.size()));
        return true;
    }

    bool CheckProvidersChanged()
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(g_providersPath.c_str(), GetFileExInfoStandard, &attributes))
        {
            if (!g_hasProviderMtime || !g_providers.empty())
            {
                g_hasProviderMtime = false;
                g_providers.clear();
                return true;
            }
            return false;
        }
        bool changed = !g_hasProviderMtime
            || CompareFileTime(&attributes.ftLastWriteTime, &g_providerMtime) != 0;
        if (changed)
        {
            g_providerMtime = attributes.ftLastWriteTime;
            g_hasProviderMtime = true;
            LoadProviders();
        }
        return changed;
    }

    bool ProviderLess(const Provider* left, const Provider* right)
    {
        if (left->lastChangeTicks != right->lastChangeTicks)
        {
            return left->lastChangeTicks > right->lastChangeTicks;
        }
        return left->sortOrder < right->sortOrder;
    }

    std::wstring ProviderAbbr(const std::string& nameUtf8)
    {
        std::wstring name = WideFromUtf8(nameUtf8);
        if (name == L"DeepSeek")
        {
            return L"DS";
        }
        if (name == L"VANKIT")
        {
            return L"VK";
        }
        if (name == L"逐梦")
        {
            return L"ZM";
        }
        if (name == L"Kimi" || name == L"Moonshot")
        {
            return L"KM";
        }
        if (name == L"One-API" || name == L"New-API")
        {
            return L"OA";
        }
        if (name == L"千问" || name == L"Token Plan" || name == L"Qianwen"
            || name == L"Qwen")
        {
            return L"QW";
        }
        if (name == L"小米" || name == L"MiMo" || name == L"Xiaomi")
        {
            return L"MI";
        }

        std::wstring abbr;
        bool lastWasLatin = false;
        for (size_t i = 0; i < name.size() && abbr.size() < 2; ++i)
        {
            wchar_t ch = name[i];
            if (IsWideChar(ch))
            {
                abbr.push_back(ch);
                lastWasLatin = false;
            }
            else if (iswalpha(ch))
            {
                if (!lastWasLatin)
                {
                    abbr.push_back(static_cast<wchar_t>(towupper(ch)));
                }
                lastWasLatin = true;
            }
            else
            {
                lastWasLatin = false;
            }
        }
        if (abbr.empty())
        {
            abbr = name.substr(0, name.size() > 2 ? 2 : name.size());
        }
        return abbr;
    }

    void WriteBalances(Provider* shown)
    {
        std::wstring line1;
        std::wstring line2;
        if (shown != nullptr)
        {
            std::wstring abbr = ProviderAbbr(shown->name);
            std::wstring balanceField;
            std::wstring spentField;
            if (IsPercentProvider(*shown))
            {
                // Token Plan 没有金额，两行都是百分比。
                line1 = abbr + L"(剩余): " + WideFromUtf8(PercentText1(shown->balance))
                    + L"%";
                line2 = abbr + (shown->weeklyPeriod ? L"(本周): " : L"(本月): ")
                    + WideFromUtf8(PercentText1(shown->spentToday)) + L"%";
            }
            else
            {
                FormatAmountPair(shown->balance, shown->spentToday, balanceField, spentField);
                line1 = abbr + L"(余额): " + balanceField;
                line2 = abbr + L"(今日): " + spentField;
            }
        }
        SetSlotTexts(line1, line2);

        std::wstring content = L"SLOT1: " + line1 + L"\r\nSLOT2: " + line2;
        wchar_t updated[64];
        swprintf(updated, 64, L"\r\nupdated=%lld\r\n", CurrentUnixSeconds());
        content += updated;

        int length = WideCharToMultiByte(936, 0, content.c_str(),
            static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes;
        if (length > 0)
        {
            bytes.resize(static_cast<size_t>(length));
            WideCharToMultiByte(936, 0, content.c_str(),
                static_cast<int>(content.size()), &bytes[0], length, nullptr, nullptr);
        }
        else
        {
            int utf8Length = WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
            bytes.resize(static_cast<size_t>(utf8Length));
            WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                static_cast<int>(content.size()), &bytes[0], utf8Length, nullptr, nullptr);
        }
        if (!WriteFileAtomic(g_balancesPath, bytes))
        {
            Log(L"写入 balances.txt 失败");
        }
    }

    void WriteState()
    {
        std::string content;
        for (size_t i = 0; i < g_providers.size(); ++i)
        {
            const Provider& provider = g_providers[i];
            content += provider.id;
            content += "|";
            if (provider.hasBalance)
            {
                content += NumberRaw(provider.balance);
            }
            content += "|";
            if (provider.lastChangeTicks != 0)
            {
                char ticks[32];
                snprintf(ticks, sizeof(ticks), "%llu", provider.lastChangeTicks);
                content += ticks;
            }
            content += "|";
            content += provider.day;
            content += "|";
            if (provider.spentToday != 0)
            {
                content += NumberRaw(provider.spentToday);
            }
            content += "\r\n";
        }
        if (!WriteFileAtomic(g_statePath, content))
        {
            Log(L"写入 bridge-state.txt 失败");
        }
    }

    // 查询线程存一份“每家当前数值”的快照，对话框只读这份快照，不直接遍历
    // g_providers，避免和查询线程抢同一份数据。
    void RebuildValueSnapshot()
    {
        std::vector<vb_options::ProviderValue> rows;
        for (size_t i = 0; i < g_providers.size(); ++i)
        {
            const Provider& provider = g_providers[i];
            vb_options::ProviderValue row;
            row.id = WideFromUtf8(provider.id);
            if (!provider.lastError.empty())
            {
                row.compact = L"失败";
            }
            else if (!provider.hasBalance)
            {
                row.compact = L"--";
            }
            else if (IsPercentProvider(provider))
            {
                row.compact = L"剩 " + WideFromUtf8(PercentText1(provider.balance)) + L"%";
            }
            else
            {
                row.compact = WideFromUtf8(AmountText2(provider.balance));
            }
            rows.push_back(row);
        }
        if (g_viewLockInitialized)
        {
            EnterCriticalSection(&g_viewLock);
            g_viewRows.swap(rows);
            LeaveCriticalSection(&g_viewLock);
        }
    }

    void RefreshOnce()
    {
        CheckProvidersChanged();

        std::string today = LocalDayString();
        int todayNumber = DayNumber(today);

        for (size_t i = 0; i < g_providers.size(); ++i)
        {
            Provider& provider = g_providers[i];
            double oldBalance = provider.balance;
            bool hadBalance = provider.hasBalance;
            std::string error;
            if (!QueryProvider(provider, error))
            {
                provider.lastError = error;
                Log(L"查询 " + WideFromUtf8(provider.name) + L" 失败：" + WideFromUtf8(error));
                continue;
            }
            provider.lastError.clear();

            if (provider.spentIsOfficial)
            {
                // VANKIT/Zhumeng and One-API expose official daily usage;
                // never mix it with the balance-drop estimate.
                provider.day = today;
            }
            else
            {
                bool countSpend = true;
                if (provider.day.empty())
                {
                    // First run after upgrading from the old state format:
                    // today's spend starts now instead of guessing from an
                    // old balance.
                    provider.spentToday = 0;
                    countSpend = false;
                }
                else if (provider.day != today)
                {
                    int previousDay = DayNumber(provider.day);
                    provider.spentToday = 0;
                    if (previousDay < 0 || todayNumber < 0 || todayNumber - previousDay > 1)
                    {
                        // More than one day was skipped; the drop cannot be
                        // attributed to today, so use the current balance as
                        // the new baseline.
                        countSpend = false;
                    }
                }
                provider.day = today;

                if (hadBalance && countSpend)
                {
                    double decrease = oldBalance - provider.balance;
                    if (decrease > 0.0000005)
                    {
                        provider.spentToday += decrease;
                    }
                }
            }

            unsigned long long now = CurrentDotNetTicks();
            if (!hadBalance || std::fabs(oldBalance - provider.balance) > 0.0000005)
            {
                provider.lastChangeTicks = now;
                if (hadBalance)
                {
                    Log(L"余额变化 " + WideFromUtf8(provider.name) + L"：" +
                        WideFromUtf8(AmountText4(oldBalance)) + L" -> " +
                        WideFromUtf8(AmountText4(provider.balance)));
                }
            }
            provider.hasBalance = true;
        }

        std::vector<Provider*> available;
        for (size_t i = 0; i < g_providers.size(); ++i)
        {
            if (g_providers[i].hasBalance && !g_providers[i].key.empty())
            {
                available.push_back(&g_providers[i]);
            }
        }
        std::stable_sort(available.begin(), available.end(), ProviderLess);

        // Only providers whose balance changed in the last 15 minutes take
        // turns. If none changed recently, keep showing the most recent one.
        const unsigned long long activeWindow = 15ULL * 60ULL * 10000000ULL;
        unsigned long long nowTicks = CurrentDotNetTicks();
        std::vector<Provider*> active;
        for (size_t i = 0; i < available.size(); ++i)
        {
            if (available[i]->lastChangeTicks == 0
                || nowTicks - available[i]->lastChangeTicks <= activeWindow)
            {
                active.push_back(available[i]);
            }
        }

        Provider* shown = nullptr;
        if (!active.empty())
        {
            if (active.size() == 1)
            {
                shown = active[0];
            }
            else
            {
                shown = active[g_rotationIndex % active.size()];
                g_rotationIndex = (g_rotationIndex + 1) % active.size();
            }
        }
        else if (!available.empty())
        {
            shown = available[0];
        }

        WriteBalances(shown);
        WriteState();
        RebuildValueSnapshot();
        g_lastRefreshTick = GetTickCount();
    }

    DWORD WINAPI WorkerProc(LPVOID)
    {
        Log(L"原生查询线程启动");
        while (true)
        {
            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
            {
                break;
            }
            RefreshOnce();
            HANDLE waits[2] = { g_stopEvent, g_reloadEvent };
            DWORD count = g_reloadEvent == nullptr ? 1 : 2;
            if (WaitForMultipleObjects(count, waits, FALSE,
                g_refreshSeconds * 1000) == WAIT_OBJECT_0)
            {
                break;
            }
        }
        Log(L"原生查询线程退出");
        return 0;
    }

    void InitPaths()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InitPaths), &module);
        wchar_t modulePath[1024];
        modulePath[0] = L'\0';
        GetModuleFileNameW(module, modulePath, 1024);
        g_module = module;
        std::wstring moduleDir = DirectoryOf(modulePath);
        std::wstring iniPath = moduleDir + L"AIBalanceNative.ini";
        g_iniPath = iniPath;
        std::wstring defaultProvidersPath = moduleDir + L"providers.json";

        wchar_t buffer[1024];
        GetPrivateProfileStringW(L"paths", L"providers", defaultProvidersPath.c_str(),
            buffer, 1024, iniPath.c_str());
        g_providersPath = buffer;
        if (g_providersPath.empty())
        {
            g_providersPath = defaultProvidersPath;
        }

        std::wstring projectDir = DirectoryOf(g_providersPath);
        GetPrivateProfileStringW(L"paths", L"log", L"",
            buffer, 1024, iniPath.c_str());
        g_logPath = buffer;
        if (g_logPath.empty())
        {
            g_logPath = projectDir + L"native.log";
        }

        g_balancesPath = projectDir + L"balances.txt";
        g_statePath = projectDir + L"bridge-state.txt";
        g_refreshSeconds = static_cast<DWORD>(GetPrivateProfileIntW(
            L"settings", L"refresh", 60, iniPath.c_str()));
        if (g_refreshSeconds < 30)
        {
            g_refreshSeconds = 30;
        }
        if (g_refreshSeconds > 3600)
        {
            g_refreshSeconds = 3600;
        }
    }

    std::string NewProviderId()
    {
        char buffer[32];
        GUID guid;
        if (CoCreateGuid(&guid) == S_OK)
        {
            snprintf(buffer, sizeof(buffer), "extra-%08lx",
                static_cast<unsigned long>(guid.Data1));
            return buffer;
        }
        snprintf(buffer, sizeof(buffer), "extra-%08lx",
            static_cast<unsigned long>(GetTickCount()));
        return buffer;
    }

    std::vector<vb_options::ProviderRow> OptionsLoadRows()
    {
        std::vector<vb_options::ProviderRow> rows;
        std::string body;
        if (!ReadFileBytes(g_providersPath, body))
        {
            return rows;
        }
        JsonValue root;
        std::string error;
        if (!ParseJson(body, root, error) || !root.IsArray())
        {
            Log(L"providers.json 解析失败：" + WideFromUtf8(error));
            return rows;
        }

        struct OrderedRow
        {
            double order;
            vb_options::ProviderRow row;
        };
        std::vector<OrderedRow> items;
        for (size_t i = 0; i < root.Size(); ++i)
        {
            const JsonValue* item = root.At(i);
            if (item == nullptr || !item->IsObject())
            {
                continue;
            }
            OrderedRow entry;
            entry.order = static_cast<double>(i);
            JsonNumber(item, "SortOrder", entry.order);
            entry.row.id = WideFromUtf8(JsonString(*item, "Id"));
            entry.row.name = WideFromUtf8(JsonString(*item, "Name"));
            entry.row.kind = WideFromUtf8(JsonString(*item, "Kind"));
            entry.row.baseUrl = WideFromUtf8(JsonString(*item, "BaseUrl"));
            std::string protectedKey = JsonString(*item, "KeyProtected");
            if (!protectedKey.empty())
            {
                std::string key;
                if (UnprotectKey(protectedKey, key))
                {
                    entry.row.key = WideFromUtf8(key);
                }
                else
                {
                    Log(L"密钥解密失败：" + entry.row.name);
                }
            }
            if (entry.row.kind.empty())
            {
                entry.row.kind = L"sub2api";
            }
            if (entry.row.name.empty())
            {
                entry.row.name = entry.row.id;
            }
            items.push_back(entry);
        }
        std::stable_sort(items.begin(), items.end(),
            [](const OrderedRow& left, const OrderedRow& right)
            {
                return left.order < right.order;
            });
        for (size_t i = 0; i < items.size(); ++i)
        {
            rows.push_back(items[i].row);
        }
        return rows;
    }

    bool OptionsSaveRows(const std::vector<vb_options::ProviderRow>& rows,
        std::wstring& error)
    {
        std::string content = "[";
        std::vector<std::string> ids;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            std::string id = Utf8FromWide(TrimSpaces(rows[i].id));
            if (id.empty())
            {
                id = NewProviderId();
            }
            for (size_t k = 0; k < ids.size(); ++k)
            {
                if (ids[k] == id)
                {
                    id = NewProviderId();
                    break;
                }
            }
            ids.push_back(id);

            std::string name = Utf8FromWide(TrimSpaces(rows[i].name));
            if (name.empty())
            {
                name = id;
            }
            std::string kind = Utf8FromWide(TrimSpaces(rows[i].kind));
            if (kind.empty())
            {
                kind = "sub2api";
            }
            std::string baseUrl = Utf8FromWide(TrimSpaces(rows[i].baseUrl));
            std::string key = Utf8FromWide(TrimSpaces(rows[i].key));
            std::string protectedKey;
            if (!ProtectKey(key, protectedKey))
            {
                error = L"密钥加密失败，请重试。";
                return false;
            }
            char order[16];
            snprintf(order, sizeof(order), "%d", static_cast<int>(i));

            if (i > 0)
            {
                content += ",";
            }
            content += "{\"Id\":\"" + JsonEscape(id)
                + "\",\"Name\":\"" + JsonEscape(name)
                + "\",\"Kind\":\"" + JsonEscape(kind)
                + "\",\"BaseUrl\":\"" + JsonEscape(baseUrl)
                + "\",\"KeyProtected\":\"" + JsonEscape(protectedKey)
                + "\",\"SortOrder\":" + order + "}";
        }
        content += "]";

        if (!WriteFileAtomic(g_providersPath, content))
        {
            error = L"写入 providers.json 失败。";
            return false;
        }
        Log(L"providers.json 已保存：" + std::to_wstring(rows.size()) + L" 家");
        if (g_reloadEvent != nullptr)
        {
            SetEvent(g_reloadEvent);
        }
        return true;
    }

    bool OptionsTestRow(const vb_options::ProviderRow& row, std::wstring& result)
    {
        Provider provider;
        provider.id = Utf8FromWide(TrimSpaces(row.id));
        provider.name = Utf8FromWide(TrimSpaces(row.name));
        provider.kind = Utf8FromWide(TrimSpaces(row.kind));
        provider.baseUrl = Utf8FromWide(TrimSpaces(row.baseUrl));
        provider.key = Utf8FromWide(TrimSpaces(row.key));
        if (provider.kind.empty())
        {
            provider.kind = "sub2api";
        }
        if (provider.name.empty())
        {
            provider.name = provider.id.empty() ? "该供应商" : provider.id;
        }
        std::string error;
        if (!QueryProvider(provider, error))
        {
            // 对话框只显示一行（超长会省略），完整原因留在日志里。
            Log(L"测试连接 " + WideFromUtf8(provider.name) + L" 失败："
                + WideFromUtf8(error));
            result = L"测试失败：" + WideFromUtf8(error);
            return false;
        }
        if (IsPercentProvider(provider))
        {
            result = L"剩余 " + WideFromUtf8(PercentText1(provider.balance))
                + L"%，连接正常。";
        }
        else
        {
            result = L"余额 " + WideFromUtf8(AmountText2(provider.balance))
                + L"，连接正常。";
        }
        return true;
    }

    std::vector<vb_options::ProviderValue> OptionsOverview()
    {
        std::vector<vb_options::ProviderValue> rows;
        if (g_viewLockInitialized)
        {
            EnterCriticalSection(&g_viewLock);
            rows = g_viewRows;
            LeaveCriticalSection(&g_viewLock);
        }
        return rows;
    }

    int OptionsHostDpi()
    {
        if (g_app == nullptr)
        {
            return 0;
        }
        return g_app->GetDPI(ITrafficMonitor::DPI_TASKBAR);
    }

    // config.ini 是 UTF-8（带 BOM），GetPrivateProfileStringW 按 ANSI 读会把中文
    // 字体名读成乱码、GDI 找不到字体就回退成宋体，所以这里自己按 UTF-8 读。
    std::wstring ReadIniValue(const std::wstring& path, const char* section,
        const char* key)
    {
        std::string body;
        if (!ReadFileBytes(path, body))
        {
            return std::wstring();
        }
        std::string currentSection;
        size_t start = 0;
        while (start <= body.size())
        {
            size_t end = body.find(static_cast<char>(10), start);
            if (end == std::string::npos)
            {
                end = body.size();
            }
            std::string line = body.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line[line.size() - 1] == static_cast<char>(13))
            {
                line.erase(line.size() - 1);
            }
            size_t first = line.find_first_not_of(" 	");
            if (first == std::string::npos || line[first] == ';')
            {
                continue;
            }
            if (line[first] == '[')
            {
                size_t close = line.find(']', first);
                currentSection = close == std::string::npos
                    ? std::string() : line.substr(first + 1, close - first - 1);
                continue;
            }
            if (currentSection != section)
            {
                continue;
            }
            size_t equal = line.find('=', first);
            if (equal == std::string::npos)
            {
                continue;
            }
            std::string name = line.substr(first, equal - first);
            while (!name.empty() && (name[name.size() - 1] == ' ' || name[name.size() - 1] == static_cast<char>(9)))
            {
                name.erase(name.size() - 1);
            }
            if (name != key)
            {
                continue;
            }
            size_t valueStart = equal + 1;
            while (valueStart < line.size() && (line[valueStart] == ' ' || line[valueStart] == static_cast<char>(9)))
            {
                ++valueStart;
            }
            return WideFromUtf8(line.substr(valueStart));
        }
        return std::wstring();
    }

    // 读 TrafficMonitor 的任务栏字体与颜色，让对话框里的示例和任务栏一致。
    void OptionsTaskbarStyle(vb_options::TaskbarStyle& style)
    {
        wchar_t appData[1024];
        appData[0] = 0;
        DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, 1024);
        if (length == 0 || length >= 1024)
        {
            return;
        }
        wchar_t separator = static_cast<wchar_t>(92);
        std::wstring iniPath = std::wstring(appData) + separator
            + L"TrafficMonitor" + separator + L"config.ini";
        if (!FileExists(iniPath))
        {
            return;
        }

        std::wstring fontName = ReadIniValue(iniPath, "task_bar", "font_name");
        if (!fontName.empty())
        {
            wcsncpy(style.fontName, fontName.c_str(), 63);
            style.fontName[63] = 0;
        }
        std::wstring sizeText = ReadIniValue(iniPath, "task_bar", "font_size");
        int size = sizeText.empty() ? 0 : _wtoi(sizeText.c_str());
        if (size >= 6 && size <= 40)
        {
            style.sizePt = size;
        }

        std::wstring styleText = ReadIniValue(iniPath, "task_bar", "font_style");
        int flags[4] = { 0, 0, 0, 0 };
        int index = 0;
        wchar_t buffer[64];
        wcsncpy(buffer, styleText.c_str(), 63);
        buffer[63] = 0;
        wchar_t* cursor = buffer;
        while (index < 4 && *cursor != 0)
        {
            flags[index++] = _wtoi(cursor);
            wchar_t* comma = wcschr(cursor, L',');
            if (comma == nullptr)
            {
                break;
            }
            cursor = comma + 1;
        }
        style.bold = flags[0] != 0;
        style.italic = flags[1] != 0;

        std::wstring backText = ReadIniValue(iniPath, "task_bar", "task_bar_back_color");
        if (!backText.empty())
        {
            style.backColor = static_cast<unsigned long>(_wtoi(backText.c_str()));
        }
        std::wstring colorText = ReadIniValue(iniPath, "task_bar", "task_bar_text_color");
        if (!colorText.empty())
        {
            style.textColor = static_cast<unsigned long>(_wtoi(colorText.c_str()));
        }
        style.ready = true;
    }

    const vb_options::OptionsHost g_optionsHost = {
        OptionsLoadRows,
        OptionsSaveRows,
        OptionsTestRow,
        Log,
        OptionsHostDpi,
        OptionsTaskbarStyle,
        OptionsOverview
    };
    volatile LONG g_optionsBusy = 0;

    bool OpenOptionsDialogForResult(void* parent)
    {
        if (InterlockedCompareExchange(&g_optionsBusy, 1, 0) != 0)
        {
            // 已经开着：把它还原并带到前台，不再新建第二个，也不静默忽略。
            if (vb_options::RaiseOpenDialog())
            {
                Log(L"供应商对话框已开着，还原并置前");
            }
            return false;
        }
        bool changed = false;
        Log(L"打开供应商对话框");
        try
        {
            changed = vb_options::ShowOptionsDialog(parent, g_optionsHost);
        }
        catch (...)
        {
            Log(L"打开供应商对话框失败");
        }
        Log(L"供应商对话框已关闭" + std::wstring(changed ? L"（已保存）" : L"（未修改）"));
        InterlockedExchange(&g_optionsBusy, 0);
        if (changed && g_reloadEvent != nullptr)
        {
            SetEvent(g_reloadEvent);
        }
        return changed;
    }

    void OpenOptionsDialog(void* parent)
    {
        OpenOptionsDialogForResult(parent);
    }

    class BalanceItem : public IPluginItem
    {
    public:
        explicit BalanceItem(int index) : m_index(index)
        {
            m_buffer[0] = L'\0';
        }

        const wchar_t* GetItemName() const override
        {
            return m_index == 0 ? L"AI 余额" : L"AI 今日消费";
        }

        const wchar_t* GetItemId() const override
        {
            return m_index == 0 ? L"vankit_slot1" : L"vankit_slot2";
        }

        const wchar_t* GetItemLableText() const override
        {
            return L"";
        }

        const wchar_t* GetItemValueText() const override
        {
            if (g_textLockInitialized)
            {
                EnterCriticalSection(&g_textLock);
                if (g_lastRefreshTick == 0
                    || GetTickCount() - g_lastRefreshTick > 900000)
                {
                    wcsncpy(m_buffer, L"--", 255);
                }
                else
                {
                    const std::wstring& text = m_index == 0 ? g_slot1Text : g_slot2Text;
                    wcsncpy(m_buffer, text.c_str(), 255);
                }
                m_buffer[255] = L'\0';
                LeaveCriticalSection(&g_textLock);
            }
            return m_buffer;
        }

        const wchar_t* GetItemValueSampleText() const override
        {
            return m_index == 0 ? L"DS(余额): 9999.99" : L"DS(今日): 9999.99";
        }

        int OnMouseEvent(MouseEventType type, int, int, void* hwnd, int) override
        {
            if (type == IPluginItem::MT_DBCLICKED)
            {
                OpenOptionsDialog(hwnd);
                return 1;
            }
            return 0;
        }

    private:
        int m_index;
        mutable wchar_t m_buffer[256];
    };

    class AIBalancePlugin : public ITMPlugin
    {
    public:
        AIBalancePlugin() : m_item1(0), m_item2(1)
        {
            InitPaths();
            InitializeCriticalSection(&g_textLock);
            g_textLockInitialized = true;
            InitializeCriticalSection(&g_viewLock);
            g_viewLockInitialized = true;
            LoadCachedBalances();
            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_reloadEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_stopEvent != nullptr)
            {
                g_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
            }
            if (g_thread == nullptr)
            {
                Log(L"创建原生查询线程失败");
            }
        }

        ~AIBalancePlugin()
        {
            if (g_stopEvent != nullptr)
            {
                SetEvent(g_stopEvent);
            }
            if (g_thread != nullptr)
            {
                WaitForSingleObject(g_thread, 30000);
                CloseHandle(g_thread);
                g_thread = nullptr;
            }
            if (g_stopEvent != nullptr)
            {
                CloseHandle(g_stopEvent);
                g_stopEvent = nullptr;
            }
            if (g_reloadEvent != nullptr)
            {
                CloseHandle(g_reloadEvent);
                g_reloadEvent = nullptr;
            }
            if (g_textLockInitialized)
            {
                DeleteCriticalSection(&g_textLock);
                g_textLockInitialized = false;
            }
            if (g_viewLockInitialized)
            {
                DeleteCriticalSection(&g_viewLock);
                g_viewLockInitialized = false;
            }
        }

        IPluginItem* GetItem(int index) override
        {
            if (index == 0)
            {
                return &m_item1;
            }
            if (index == 1)
            {
                return &m_item2;
            }
            return nullptr;
        }

        void DataRequired() override
        {
        }

        const wchar_t* GetInfo(PluginInfoIndex index) override
        {
            switch (index)
            {
            case TMI_NAME:
                return L"TrafficMonitor AI Balance";
            case TMI_DESCRIPTION:
                return L"在 TrafficMonitor 进程内查询 Sub2API/DeepSeek/Kimi/One-API 余额，供应商管理界面也内置在插件里。";
            case TMI_AUTHOR:
                return L"StephenBoyMi";
            case TMI_COPYRIGHT:
                return L"© 2026 StephenBoyMi";
            case TMI_VERSION:
                return L"2.2.2";
            case TMI_URL:
                return L"https://github.com/StephenBoyMi/trafficmonitor-ai-balance";
            default:
                return L"";
            }
        }

        void* GetPluginIcon() override
        {
            static HICON icon = g_module != nullptr
                ? LoadIconW(g_module, MAKEINTRESOURCEW(1)) : nullptr;
            return icon;
        }

        OptionReturn ShowOptionsDialog(void* hParent) override
        {
            return OpenOptionsDialogForResult(hParent)
                ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
        }

        void OnInitialize(ITrafficMonitor* app) override
        {
            g_app = app;
        }

        const wchar_t* GetTooltipInfo() override
        {
            return L"TrafficMonitor AI Balance 运行中";
        }

    private:
        BalanceItem m_item1;
        BalanceItem m_item2;
    };
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    static AIBalancePlugin plugin;
    return &plugin;
}
