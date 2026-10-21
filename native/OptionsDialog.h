#pragma once

#include <cwchar>
#include <string>
#include <vector>

namespace vb_options
{
    // 对话框内部只使用这一种供应商行结构：密钥是明文，只在对话框存活期间存在。
    struct ProviderRow
    {
        std::wstring id;
        std::wstring name;
        std::wstring kind;      // sub2api / deepseek / moonshot / oneapi
        std::wstring baseUrl;
        std::wstring key;
    };

    // 任务栏当前的外观，供对话框里的“任务栏示例”按原样绘制。
    struct TaskbarStyle
    {
        wchar_t fontName[64];
        int sizePt;
        bool bold;
        bool italic;
        unsigned long textColor;    // COLORREF
        unsigned long backColor;    // COLORREF
        bool ready;                 // 是否成功读到配置

        TaskbarStyle()
            : sizePt(10), bold(false), italic(false),
              textColor(0x000000FF), backColor(0x00F1E7D7), ready(false)
        {
            fontName[0] = L'\0';
            wcscpy(fontName, L"Microsoft YaHei");
        }
    };

    // 查询线程快照里的一行：左侧列表按 Id 取用，只显示一个短数值。
    struct ProviderValue
    {
        std::wstring id;
        std::wstring compact;   // 如 "1.03"、"剩 72.2%"、"失败"
    };

    // 由原生插件（AIBalanceNative.cpp）实现的回调集合。
    // 约定：除 testRow 在工作线程调用外，其余都在对话框线程调用；
    //      所有回调都不得读写插件的 g_providers，避免与查询线程竞争。
    // 未赋值的成员保持 nullptr，对话框会跳过对应功能。
    struct OptionsHost
    {
        std::vector<ProviderRow> (*loadRows)() = nullptr;
        bool (*saveRows)(const std::vector<ProviderRow>& rows, std::wstring& error) = nullptr;
        bool (*testRow)(const ProviderRow& row, std::wstring& result) = nullptr;
        void (*log)(const std::wstring& message) = nullptr;
        int (*hostDpi)() = nullptr;     // 宿主窗口的 DPI，取不到时返回 0
        void (*taskbarStyle)(TaskbarStyle& style) = nullptr;   // 读宿主任务栏外观
        // 读查询线程的快照，用于在左侧列表里带上当前数值。
        std::vector<ProviderValue> (*overview)() = nullptr;
    };

    // 以模态方式打开对话框。
    // 返回 true 表示用户在对话框里保存过设置。
    bool ShowOptionsDialog(void* hParent, const OptionsHost& host);
}
