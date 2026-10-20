#pragma once
#include <map>
#include <string>
#include <vector>

class JsonValue
{
public:
    enum Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Type type;
    bool boolValue;
    double numberValue;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    JsonValue();

    bool IsNull() const { return type == Null; }
    bool IsObject() const { return type == Object; }
    bool IsArray() const { return type == Array; }
    bool IsString() const { return type == String; }
    bool IsNumber() const { return type == Number; }
    bool IsBool() const { return type == Bool; }

    const JsonValue* Get(const std::string& key) const;
    const JsonValue* At(size_t index) const;
    size_t Size() const;
    double AsNumber(double fallback) const;
    std::string AsString(const std::string& fallback) const;
    bool AsBool(bool fallback) const;
};

bool ParseJson(const std::string& text, JsonValue& out, std::string& error);
