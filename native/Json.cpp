#include "Json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

JsonValue::JsonValue()
    : type(Null), boolValue(false), numberValue(0)
{
}

const JsonValue* JsonValue::Get(const std::string& key) const
{
    if (type != Object)
    {
        return nullptr;
    }
    std::map<std::string, JsonValue>::const_iterator it = objectValue.find(key);
    if (it == objectValue.end())
    {
        return nullptr;
    }
    return &it->second;
}

const JsonValue* JsonValue::At(size_t index) const
{
    if (type != Array || index >= arrayValue.size())
    {
        return nullptr;
    }
    return &arrayValue[index];
}

size_t JsonValue::Size() const
{
    if (type == Array)
    {
        return arrayValue.size();
    }
    if (type == Object)
    {
        return objectValue.size();
    }
    return 0;
}

double JsonValue::AsNumber(double fallback) const
{
    if (type == Number)
    {
        return numberValue;
    }
    if (type == String)
    {
        char* end = nullptr;
        double value = std::strtod(stringValue.c_str(), &end);
        if (end != stringValue.c_str())
        {
            return value;
        }
    }
    return fallback;
}

std::string JsonValue::AsString(const std::string& fallback) const
{
    if (type == String)
    {
        return stringValue;
    }
    return fallback;
}

bool JsonValue::AsBool(bool fallback) const
{
    if (type == Bool)
    {
        return boolValue;
    }
    if (type == Number)
    {
        return numberValue != 0;
    }
    if (type == String)
    {
        if (stringValue == "true")
        {
            return true;
        }
        if (stringValue == "false")
        {
            return false;
        }
    }
    return fallback;
}

namespace
{
    class Parser
    {
    public:
        Parser(const std::string& text)
            : m_text(text), m_pos(0), m_depth(0)
        {
        }

        bool Parse(JsonValue& out, std::string& error)
        {
            SkipSpace();
            if (!ParseValue(out))
            {
                error = m_error;
                return false;
            }
            SkipSpace();
            if (m_pos != m_text.size())
            {
                error = Error("多余的字符");
                return false;
            }
            return true;
        }

    private:
        const std::string& m_text;
        size_t m_pos;
        int m_depth;
        std::string m_error;

        std::string Error(const char* message) const
        {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "%s（位置 %lu）", message,
                static_cast<unsigned long>(m_pos));
            return std::string(buffer);
        }

        void SkipSpace()
        {
            while (m_pos < m_text.size())
            {
                char c = m_text[m_pos];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    ++m_pos;
                }
                else
                {
                    break;
                }
            }
        }

        bool ParseValue(JsonValue& out)
        {
            if (++m_depth > 64)
            {
                m_error = Error("JSON 嵌套过深");
                return false;
            }
            SkipSpace();
            if (m_pos >= m_text.size())
            {
                m_error = Error("JSON 意外结束");
                return false;
            }

            bool ok = false;
            char c = m_text[m_pos];
            if (c == '{')
            {
                ok = ParseObject(out);
            }
            else if (c == '[')
            {
                ok = ParseArray(out);
            }
            else if (c == '"')
            {
                out.type = JsonValue::String;
                ok = ParseString(out.stringValue);
            }
            else if (c == 't' && Match("true"))
            {
                out.type = JsonValue::Bool;
                out.boolValue = true;
                ok = true;
            }
            else if (c == 'f' && Match("false"))
            {
                out.type = JsonValue::Bool;
                out.boolValue = false;
                ok = true;
            }
            else if (c == 'n' && Match("null"))
            {
                out.type = JsonValue::Null;
                ok = true;
            }
            else if (c == '-' || (c >= '0' && c <= '9'))
            {
                ok = ParseNumber(out);
            }
            else
            {
                m_error = Error("无法识别的 JSON 值");
            }

            --m_depth;
            return ok;
        }

        bool Match(const char* literal)
        {
            size_t length = std::strlen(literal);
            if (m_text.compare(m_pos, length, literal) != 0)
            {
                return false;
            }
            m_pos += length;
            return true;
        }

        bool ParseObject(JsonValue& out)
        {
            out.type = JsonValue::Object;
            ++m_pos;
            SkipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == '}')
            {
                ++m_pos;
                return true;
            }

            while (m_pos < m_text.size())
            {
                SkipSpace();
                if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                {
                    m_error = Error("对象的键必须是字符串");
                    return false;
                }
                std::string key;
                if (!ParseString(key))
                {
                    return false;
                }
                SkipSpace();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                {
                    m_error = Error("对象键后缺少冒号");
                    return false;
                }
                ++m_pos;
                JsonValue value;
                if (!ParseValue(value))
                {
                    return false;
                }
                out.objectValue[key] = value;
                SkipSpace();
                if (m_pos >= m_text.size())
                {
                    m_error = Error("对象意外结束");
                    return false;
                }
                if (m_text[m_pos] == ',')
                {
                    ++m_pos;
                    continue;
                }
                if (m_text[m_pos] == '}')
                {
                    ++m_pos;
                    return true;
                }
                m_error = Error("对象缺少逗号或右花括号");
                return false;
            }
            m_error = Error("对象意外结束");
            return false;
        }

        bool ParseArray(JsonValue& out)
        {
            out.type = JsonValue::Array;
            ++m_pos;
            SkipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ']')
            {
                ++m_pos;
                return true;
            }

            while (m_pos < m_text.size())
            {
                JsonValue value;
                if (!ParseValue(value))
                {
                    return false;
                }
                out.arrayValue.push_back(value);
                SkipSpace();
                if (m_pos >= m_text.size())
                {
                    m_error = Error("数组意外结束");
                    return false;
                }
                if (m_text[m_pos] == ',')
                {
                    ++m_pos;
                    continue;
                }
                if (m_text[m_pos] == ']')
                {
                    ++m_pos;
                    return true;
                }
                m_error = Error("数组缺少逗号或右方括号");
                return false;
            }
            m_error = Error("数组意外结束");
            return false;
        }

        bool ParseString(std::string& out)
        {
            ++m_pos;
            out.clear();
            while (m_pos < m_text.size())
            {
                unsigned char c = static_cast<unsigned char>(m_text[m_pos]);
                if (c == '"')
                {
                    ++m_pos;
                    return true;
                }
                if (c == '\\')
                {
                    ++m_pos;
                    if (m_pos >= m_text.size())
                    {
                        m_error = Error("字符串转义不完整");
                        return false;
                    }
                    char esc = m_text[m_pos++];
                    switch (esc)
                    {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                    {
                        unsigned int code = 0;
                        if (!ParseHex4(code))
                        {
                            return false;
                        }
                        if (code >= 0xD800 && code <= 0xDBFF && m_pos + 1 < m_text.size()
                            && m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u')
                        {
                            m_pos += 2;
                            unsigned int low = 0;
                            if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF)
                            {
                                m_error = Error("无效的 UTF-16 代理对");
                                return false;
                            }
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }
                        AppendUtf8(out, code);
                        break;
                    }
                    default:
                        m_error = Error("未知的字符串转义");
                        return false;
                    }
                }
                else
                {
                    out.push_back(static_cast<char>(c));
                    ++m_pos;
                }
            }
            m_error = Error("字符串意外结束");
            return false;
        }

        bool ParseHex4(unsigned int& value)
        {
            if (m_pos + 4 > m_text.size())
            {
                m_error = Error("\\u 转义不足 4 位");
                return false;
            }
            value = 0;
            for (int i = 0; i < 4; ++i)
            {
                char c = m_text[m_pos++];
                value <<= 4;
                if (c >= '0' && c <= '9') value += c - '0';
                else if (c >= 'a' && c <= 'f') value += c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') value += c - 'A' + 10;
                else
                {
                    m_error = Error("\\u 转义含非十六进制字符");
                    return false;
                }
            }
            return true;
        }

        static void AppendUtf8(std::string& out, unsigned int code)
        {
            if (code <= 0x7F)
            {
                out.push_back(static_cast<char>(code));
            }
            else if (code <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else if (code <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }

        bool ParseNumber(JsonValue& out)
        {
            size_t start = m_pos;
            if (m_pos < m_text.size() && m_text[m_pos] == '-')
            {
                ++m_pos;
            }
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
            {
                ++m_pos;
            }
            if (m_pos < m_text.size() && m_text[m_pos] == '.')
            {
                ++m_pos;
                while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                {
                    ++m_pos;
                }
            }
            if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
            {
                ++m_pos;
                if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
                {
                    ++m_pos;
                }
                while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                {
                    ++m_pos;
                }
            }
            if (start == m_pos)
            {
                m_error = Error("无效的数字");
                return false;
            }
            std::string number = m_text.substr(start, m_pos - start);
            char* end = nullptr;
            double value = std::strtod(number.c_str(), &end);
            if (end == number.c_str())
            {
                m_error = Error("无效的数字");
                return false;
            }
            out.type = JsonValue::Number;
            out.numberValue = value;
            return true;
        }
    };
}

bool ParseJson(const std::string& text, JsonValue& out, std::string& error)
{
    Parser parser(text);
    return parser.Parse(out, error);
}
