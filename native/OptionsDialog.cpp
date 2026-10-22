#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <new>
#include <string>
#include <vector>

#include "OptionsDialog.h"

namespace vb_options
{
    namespace
    {
        enum ControlIds
        {
            IDC_LIST = 1001,
            IDC_ADD,
            IDC_DELETE,
            IDC_NAME = 1006,
            IDC_KIND,
            IDC_URL,
            IDC_KEY,
            IDC_SHOW_KEY,
            IDC_TEST,
            IDC_SAMPLE,
            IDC_STATUS,
            IDC_SAVE,
            IDC_CLOSE,
            IDC_HINT
        };

        const wchar_t* kWindowClass = L"AIBalanceOptionsWindow";
        const wchar_t* kWindowTitle = L"供应商设置";

        // 版式沿用原管理器：620×480 窗口、左侧窄、三个分组框。
        const int kClientWidth = 620;
        const int kClientHeight = 450;
        // 18 DIP 下系统默认的 U+25CF 圆点过大，改用小圆点。
        const wchar_t kPasswordChar = 0x2022;
        // Token Plan 类型不需要 Base URL，这一格显示灰字“留空”并禁用。
        const wchar_t kLeaveBlankText[] = L"留空";

        const COLORREF kColorText = RGB(0x11, 0x18, 0x27);
        const COLORREF kColorMuted = RGB(0x6B, 0x72, 0x80);
        const COLORREF kColorOk = RGB(0x15, 0x80, 0x3D);
        const COLORREF kColorError = RGB(0xB9, 0x1C, 0x1C);

        const UINT_PTR kTestTimer = 1;

        struct KindOption
        {
            const wchar_t* label;
            const wchar_t* kind;
        };

        const KindOption kKinds[] = {
            { L"Sub2API 中转", L"sub2api" },
            { L"DeepSeek 官方", L"deepseek" },
            { L"Kimi 官方", L"moonshot" },
            { L"One-API / New-API", L"oneapi" },
            { L"千问 Token Plan（Cookie）", L"token_plan" },
            { L"小米 Token Plan（Cookie）", L"xiaomi" }
        };
        const int kKindCount = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));

        // 测试连接的结果块：对话框与后台线程各持一次引用，最后释放者负责删除。
        struct TestBlock
        {
            CRITICAL_SECTION lock;
            volatile LONG references;
            bool finished;
            bool ok;
            std::wstring text;
            ProviderRow row;
            bool (*testRow)(const ProviderRow&, std::wstring&);

            TestBlock() : references(0), finished(false), ok(false), testRow(nullptr)
            {
                InitializeCriticalSection(&lock);
            }

            ~TestBlock()
            {
                DeleteCriticalSection(&lock);
            }
        };

        void AddReference(TestBlock* block)
        {
            InterlockedIncrement(&block->references);
        }

        void ReleaseReference(TestBlock* block)
        {
            if (InterlockedDecrement(&block->references) == 0)
            {
                delete block;
            }
        }

        struct DialogState
        {
            DialogState()
                : instance(nullptr), hwnd(nullptr), list(nullptr), addButton(nullptr),
                  deleteButton(nullptr),
                  nameEdit(nullptr), kindCombo(nullptr), urlEdit(nullptr), keyEdit(nullptr),
                  showKeyCheck(nullptr), testButton(nullptr), sample(nullptr),
                  status(nullptr), saveButton(nullptr), selected(-1), loading(false),
                  saved(false), destroyed(false), test(nullptr), dragging(false),
                  dragFrom(-1), dragTarget(-1),
                  taskbarLabel(nullptr), sampleLabel(nullptr), statusColor(kColorMuted),
                  font(nullptr), hintFont(nullptr), sampleFont(nullptr), dpi(96)
            {
            }

            HINSTANCE instance;
            HWND hwnd;
            HWND list;
            HWND addButton;
            HWND deleteButton;
            HWND nameEdit;
            HWND kindCombo;
            HWND urlEdit;
            HWND keyEdit;
            HWND showKeyCheck;
            HWND testButton;
            HWND sample;
            HWND status;
            HWND saveButton;

            OptionsHost host;
            TaskbarStyle style;
            std::vector<ProviderRow> rows;
            // 与 rows 同序的当前数值，RefreshList 每次重建时刷新。
            std::vector<std::wstring> values;
            int selected;
            bool loading;
            bool saved;
            bool destroyed;
            TestBlock* test;
            bool dragging;
            int dragFrom;
            int dragTarget;
            HWND taskbarLabel;
            HWND sampleLabel;
            COLORREF statusColor;

            HFONT font;
            HFONT hintFont;
            HFONT sampleFont;
            UINT dpi;
        };

        DialogState* StateOf(HWND hwnd)
        {
            return reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        int Scaled(DialogState* state, int value)
        {
            return static_cast<int>((static_cast<long long>(value) * state->dpi + 48) / 96);
        }

        HFONT MakeFont(UINT dpi, int pointSize, DWORD weight, const wchar_t* face, bool italic)
        {
            LOGFONTW font;
            ZeroMemory(&font, sizeof(font));
            font.lfHeight = -MulDiv(pointSize, static_cast<int>(dpi), 72);
            font.lfWeight = weight;
            font.lfItalic = italic ? TRUE : FALSE;
            font.lfCharSet = DEFAULT_CHARSET;
            font.lfQuality = CLEARTYPE_QUALITY;
            wcsncpy(font.lfFaceName, face, LF_FACESIZE - 1);
            font.lfFaceName[LF_FACESIZE - 1] = L'\0';
            return CreateFontIndirectW(&font);
        }

        HFONT MakeFontDip(UINT dpi, int dipSize, DWORD weight, const wchar_t* face, bool italic)
        {
            LOGFONTW font;
            ZeroMemory(&font, sizeof(font));
            font.lfHeight = -MulDiv(dipSize, static_cast<int>(dpi), 96);
            font.lfWeight = weight;
            font.lfItalic = italic ? TRUE : FALSE;
            font.lfCharSet = DEFAULT_CHARSET;
            font.lfQuality = CLEARTYPE_QUALITY;
            wcsncpy(font.lfFaceName, face, LF_FACESIZE - 1);
            font.lfFaceName[LF_FACESIZE - 1] = 0;
            return CreateFontIndirectW(&font);
        }

        HWND MakeControl(DialogState* state, const wchar_t* className,
            const wchar_t* text, DWORD style, DWORD exStyle, int x, int y, int width,
            int height, int id, HFONT font)
        {
            HWND control = CreateWindowExW(exStyle, className, text,
                WS_CHILD | WS_VISIBLE | style,
                Scaled(state, x), Scaled(state, y),
                Scaled(state, width), Scaled(state, height),
                state->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                state->instance, nullptr);
            if (control != nullptr && font != nullptr)
            {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
            return control;
        }

        std::wstring KindLabel(const std::wstring& kind)
        {
            for (int i = 0; i < kKindCount; ++i)
            {
                if (kKinds[i].kind == kind)
                {
                    return kKinds[i].label;
                }
            }
            return kind;
        }

        // Key 这一格对 Token Plan 类型存的是浏览器 Cookie，提示按类型给。
        std::wstring KeyHint(const std::wstring& kind, bool hasValue)
        {
            if (kind == L"token_plan")
            {
                return hasValue ? L"已保存 Cookie；清空后保存即删除。"
                    : L"请填写浏览器请求头里的 Cookie。";
            }
            return hasValue ? L"已保存 key；清空后保存即删除。"
                : L"尚未设置 key。";
        }

        int KindIndex(const std::wstring& kind)
        {
            for (int i = 0; i < kKindCount; ++i)
            {
                if (kKinds[i].kind == kind)
                {
                    return i;
                }
            }
            return 0;
        }

        std::wstring EditText(HWND edit)
        {
            int length = GetWindowTextLengthW(edit);
            if (length <= 0)
            {
                return std::wstring();
            }
            std::wstring text;
            text.resize(static_cast<size_t>(length) + 1);
            GetWindowTextW(edit, &text[0], length + 1);
            text.resize(static_cast<size_t>(length));
            return text;
        }

        bool IsCookieKind(const std::wstring& kind)
        {
            return kind == L"token_plan" || kind == L"xiaomi";
        }

        void ApplyUrlFieldMode(DialogState* state, const std::wstring& kind, bool enabled)
        {
            if (state->urlEdit == nullptr)
            {
                return;
            }
            if (IsCookieKind(kind))
            {
                SetWindowTextW(state->urlEdit, kLeaveBlankText);
                EnableWindow(state->urlEdit, FALSE);
                return;
            }
            // 从 Cookie 类型切回来时把占位文字清掉，避免当成真实地址保存。
            if (EditText(state->urlEdit) == kLeaveBlankText)
            {
                SetWindowTextW(state->urlEdit, L"");
            }
            EnableWindow(state->urlEdit, enabled ? TRUE : FALSE);
        }

        void SetStatus(DialogState* state, const std::wstring& text, COLORREF color)
        {
            state->statusColor = color;
            if (state->status != nullptr)
            {
                SetWindowTextW(state->status, text.c_str());
                InvalidateRect(state->status, nullptr, TRUE);
            }
        }

        int SelectedIndex(DialogState* state)
        {
            return static_cast<int>(SendMessageW(state->list, LVM_GETNEXTITEM,
                static_cast<WPARAM>(-1), LVNI_SELECTED));
        }

        // 左侧列表名称后面带上当前数值；按 Id 对齐查询线程的快照。
        std::wstring CompactValueFor(const ProviderRow& row,
            const std::vector<ProviderValue>& view)
        {
            if (row.id.empty())
            {
                return std::wstring();
            }
            for (size_t i = 0; i < view.size(); ++i)
            {
                if (view[i].id == row.id && !view[i].compact.empty())
                {
                    return view[i].compact;
                }
            }
            return std::wstring();
        }

        void RefreshList(DialogState* state)
        {
            SendMessageW(state->list, LVM_DELETEALLITEMS, 0, 0);
            std::vector<ProviderValue> view;
            if (state->host.overview != nullptr)
            {
                view = state->host.overview();
            }
            state->values.assign(state->rows.size(), std::wstring());
            for (size_t i = 0; i < state->rows.size(); ++i)
            {
                const ProviderRow& row = state->rows[i];
                std::wstring text = row.name.empty() ? L"(未命名)" : row.name;
                std::wstring& compact = state->values[i];
                compact = CompactValueFor(row, view);
                if (!compact.empty())
                {
                    text += L" · " + compact;
                }

                LVITEMW item;
                ZeroMemory(&item, sizeof(item));
                item.mask = LVIF_TEXT;
                item.iItem = static_cast<int>(i);
                item.iSubItem = 0;
                item.pszText = &text[0];
                SendMessageW(state->list, LVM_INSERTITEMW, 0,
                    reinterpret_cast<LPARAM>(&item));
            }

            int count = static_cast<int>(state->rows.size());
            if (state->selected >= count)
            {
                state->selected = count - 1;
            }
            if (state->selected < 0 && count > 0)
            {
                state->selected = 0;
            }
            if (state->selected >= 0)
            {
                LVITEMW item;
                ZeroMemory(&item, sizeof(item));
                item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                item.state = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageW(state->list, LVM_SETITEMSTATE,
                    static_cast<WPARAM>(state->selected), reinterpret_cast<LPARAM>(&item));
                SendMessageW(state->list, LVM_ENSUREVISIBLE,
                    static_cast<WPARAM>(state->selected), FALSE);
            }

            bool hasSelection = state->selected >= 0 && state->selected < count;
            BOOL enable = hasSelection ? TRUE : FALSE;
            EnableWindow(state->deleteButton, enable);
            EnableWindow(state->nameEdit, enable);
            EnableWindow(state->kindCombo, enable);
            EnableWindow(state->urlEdit, enable);
            if (hasSelection)
            {
                ApplyUrlFieldMode(state, state->rows[state->selected].kind, true);
            }
            EnableWindow(state->keyEdit, enable);
            EnableWindow(state->showKeyCheck, enable);
            EnableWindow(state->testButton, state->test == nullptr ? enable : FALSE);
        }

        // 重建列表时屏蔽选中变化通知：增删行会让 ListView 先发出
        // “全部取消选中”，那时若被当成用户切换，会把表单内容写进新行。
        void RefreshListQuiet(DialogState* state)
        {
            bool wasLoading = state->loading;
            state->loading = true;
            RefreshList(state);
            state->loading = wasLoading;
        }

        void UpdateSelectedTitle(DialogState* state)
        {
            if (state->selected < 0 || state->selected >= static_cast<int>(state->rows.size()))
            {
                return;
            }
            const ProviderRow& row = state->rows[state->selected];
            std::wstring name = row.name.empty() ? L"(未命名)" : row.name;
            if (state->selected < static_cast<int>(state->values.size())
                && !state->values[state->selected].empty())
            {
                name += L" · " + state->values[state->selected];
            }
            LVITEMW item;
            ZeroMemory(&item, sizeof(item));
            item.iSubItem = 0;
            item.pszText = &name[0];
            SendMessageW(state->list, LVM_SETITEMTEXTW,
                static_cast<WPARAM>(state->selected), reinterpret_cast<LPARAM>(&item));
        }

        void LoadRowIntoControls(DialogState* state)
        {
            state->loading = true;
            if (state->selected >= 0 && state->selected < static_cast<int>(state->rows.size()))
            {
                const ProviderRow& row = state->rows[state->selected];
                SetWindowTextW(state->nameEdit, row.name.c_str());
                SendMessageW(state->kindCombo, CB_SETCURSEL,
                    static_cast<WPARAM>(KindIndex(row.kind)), 0);
                SetWindowTextW(state->keyEdit, row.key.c_str());
                // Cookie 类型的占位“留空”由 ApplyUrlFieldMode 统一写，避免两处重复。
                SetWindowTextW(state->urlEdit, IsCookieKind(row.kind)
                    ? L"" : row.baseUrl.c_str());
                ApplyUrlFieldMode(state, row.kind, true);
            }
            else
            {
                SetWindowTextW(state->nameEdit, L"");
                SendMessageW(state->kindCombo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                SetWindowTextW(state->urlEdit, L"");
                SetWindowTextW(state->keyEdit, L"");
            }
            state->loading = false;
            if (state->selected >= 0 && state->selected < static_cast<int>(state->rows.size()))
            {
                const ProviderRow& row = state->rows[state->selected];
                SetStatus(state, KeyHint(row.kind, !row.key.empty()), kColorMuted);
            }
            else
            {
                SetStatus(state, L"", kColorMuted);
            }
        }

        void CommitControlsToRow(DialogState* state)
        {
            if (state->selected < 0 || state->selected >= static_cast<int>(state->rows.size()))
            {
                return;
            }
            ProviderRow& row = state->rows[state->selected];
            row.name = EditText(state->nameEdit);
            row.key = EditText(state->keyEdit);
            int kind = static_cast<int>(SendMessageW(state->kindCombo, CB_GETCURSEL, 0, 0));
            if (kind >= 0 && kind < kKindCount)
            {
                row.kind = kKinds[kind].kind;
            }
            // Cookie 类型的这一格是禁用的占位“留空”，不参与提交；
            // 原有地址保留在行里，切回其他类型时还能恢复。
            std::wstring urlText = EditText(state->urlEdit);
            if (!IsCookieKind(row.kind) && urlText != kLeaveBlankText)
            {
                row.baseUrl = urlText;
            }
        }

        void DrawSample(DialogState* state, HDC dc, const RECT& box)
        {
            COLORREF back = state->style.ready ? state->style.backColor : RGB(0xF1, 0xE7, 0xD7);
            HBRUSH brush = CreateSolidBrush(back);
            HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
            HGDIOBJ oldBrush = SelectObject(dc, brush);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            RoundRect(dc, box.left, box.top, box.right, box.bottom,
                Scaled(state, 6), Scaled(state, 6));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(brush);
            DeleteObject(pen);

            RECT inner = box;
            inner.left += Scaled(state, 4);
            inner.right -= Scaled(state, 4);
            SetBkMode(dc, TRANSPARENT);
            HFONT font = state->sampleFont != nullptr ? state->sampleFont : state->font;
            HGDIOBJ oldFont = SelectObject(dc, font);


            // 示例全部使用固定演示值，不读实时数据。
            std::wstring balance1Text = L"DS(余额): 72.30";
            std::wstring balance2Text = L"DS(今日):   0.07";
            std::wstring upText = L"↑: 2.6 M/s";
            std::wstring downText = L"↓: 2.6 M/s";
            std::wstring cpuText = L"CPU: 51 %";
            std::wstring memoryText = L"内存: 63 %";

            SIZE sample = { 0, 0 };
            SIZE sample2 = { 0, 0 };
            SIZE sizeUp = { 0, 0 };
            SIZE sizeDown = { 0, 0 };
            SIZE sizeCpu = { 0, 0 };
            SIZE sizeMemory = { 0, 0 };
            GetTextExtentPoint32W(dc, balance1Text.c_str(),
                static_cast<int>(balance1Text.size()), &sample);
            GetTextExtentPoint32W(dc, balance2Text.c_str(),
                static_cast<int>(balance2Text.size()), &sample2);
            GetTextExtentPoint32W(dc, upText.c_str(), static_cast<int>(upText.size()), &sizeUp);
            GetTextExtentPoint32W(dc, downText.c_str(), static_cast<int>(downText.size()), &sizeDown);
            GetTextExtentPoint32W(dc, cpuText.c_str(), static_cast<int>(cpuText.size()), &sizeCpu);
            GetTextExtentPoint32W(dc, memoryText.c_str(), static_cast<int>(memoryText.size()), &sizeMemory);

            int lineHeight = sample.cy > 0 ? sample.cy : Scaled(state, 18);
            int lineGap = Scaled(state, 2);
            int blockHeight = lineHeight * 2 + lineGap;
            int top = inner.top + ((inner.bottom - inner.top) - blockHeight) / 2;

            // 三列宽度全部按实际文字量出来，再顺序排开并整体居中，避免相互压字。
            int widthUp = sizeUp.cx > sizeDown.cx ? sizeUp.cx : sizeDown.cx;
            int widthCpu = sizeCpu.cx > sizeMemory.cx ? sizeCpu.cx : sizeMemory.cx;
            int widthBalance = sample.cx > sample2.cx ? sample.cx : sample2.cx;
            int columnGap = Scaled(state, 10);
            int totalWidth = widthUp + widthCpu + widthBalance + columnGap * 2;
            int innerWidth = inner.right - inner.left;
            if (totalWidth > innerWidth)
            {
                columnGap = (innerWidth - widthUp - widthCpu - widthBalance) / 2;
                if (columnGap < Scaled(state, 4))
                {
                    columnGap = Scaled(state, 4);
                }
                totalWidth = widthUp + widthCpu + widthBalance + columnGap * 2;
            }
            int startX = inner.left + (innerWidth - totalWidth) / 2;
            if (startX < inner.left)
            {
                startX = inner.left;
            }
            int upX = startX;
            int cpuX = upX + widthUp + columnGap;
            int balanceX = cpuX + widthCpu + columnGap;

            SetTextColor(dc, state->style.ready ? state->style.textColor : RGB(0xC0, 0x1B, 0x1B));
            RECT upRect = { upX, top, cpuX, top + lineHeight };
            RECT downRect = { upX, top + lineHeight + lineGap, cpuX, top + blockHeight };
            RECT cpuRect = { cpuX, top, balanceX, top + lineHeight };
            RECT memoryRect = { cpuX, top + lineHeight + lineGap, balanceX, top + blockHeight };
            DrawTextW(dc, upText.c_str(), -1, &upRect,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
            DrawTextW(dc, downText.c_str(), -1, &downRect,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
            DrawTextW(dc, cpuText.c_str(), -1, &cpuRect,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
            DrawTextW(dc, memoryText.c_str(), -1, &memoryRect,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);

            RECT balance1 = { balanceX, top, balanceX + widthBalance, top + lineHeight };
            RECT balance2 = { balanceX, top + lineHeight + lineGap,
                balanceX + widthBalance, top + blockHeight };
            DrawTextW(dc, balance1Text.c_str(), -1, &balance1,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
            DrawTextW(dc, balance2Text.c_str(), -1, &balance2,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        }

        DWORD WINAPI TestThreadProc(LPVOID parameter)
        {
            TestBlock* block = static_cast<TestBlock*>(parameter);
            std::wstring text;
            bool ok = block->testRow != nullptr && block->testRow(block->row, text);
            EnterCriticalSection(&block->lock);
            block->ok = ok;
            block->text = text;
            block->finished = true;
            LeaveCriticalSection(&block->lock);
            ReleaseReference(block);
            return 0;
        }

        void StartTest(DialogState* state)
        {
            if (state->test != nullptr)
            {
                SetStatus(state, L"上一次测试仍在进行，请稍候。", kColorMuted);
                return;
            }
            CommitControlsToRow(state);
            if (state->selected < 0 || state->selected >= static_cast<int>(state->rows.size()))
            {
                return;
            }
            TestBlock* block = new TestBlock();
            block->row = state->rows[state->selected];
            block->testRow = state->host.testRow;
            AddReference(block);
            AddReference(block);
            state->test = block;
            EnableWindow(state->testButton, FALSE);
            SetStatus(state, L"正在测试连接……", kColorMuted);

            HANDLE thread = CreateThread(nullptr, 0, TestThreadProc, block, 0, nullptr);
            if (thread == nullptr)
            {
                state->test = nullptr;
                ReleaseReference(block);
                ReleaseReference(block);
                EnableWindow(state->testButton, TRUE);
                SetStatus(state, L"无法启动测试线程。", kColorError);
                return;
            }
            CloseHandle(thread);
            SetTimer(state->hwnd, kTestTimer, 250, nullptr);
        }

        void PollTest(DialogState* state)
        {
            TestBlock* block = state->test;
            if (block == nullptr)
            {
                KillTimer(state->hwnd, kTestTimer);
                return;
            }
            bool finished = false;
            bool ok = false;
            std::wstring text;
            EnterCriticalSection(&block->lock);
            finished = block->finished;
            if (finished)
            {
                ok = block->ok;
                text = block->text;
            }
            LeaveCriticalSection(&block->lock);
            if (!finished)
            {
                return;
            }

            KillTimer(state->hwnd, kTestTimer);
            state->test = nullptr;
            bool hasSelection = state->selected >= 0
                && state->selected < static_cast<int>(state->rows.size());
            EnableWindow(state->testButton, hasSelection ? TRUE : FALSE);
            if (text.empty())
            {
                text = ok ? L"测试通过。" : L"测试失败。";
            }
            SetStatus(state, text, ok ? kColorOk : kColorError);
            if (state->host.log != nullptr)
            {
                state->host.log(L"测试连接：" + text);
            }
            ReleaseReference(block);
        }

        void OnNew(DialogState* state)
        {
            CommitControlsToRow(state);
            ProviderRow row;
            row.id.clear();
            // 新增一行就是全空，不预填名字；列表里显示“(未命名)”。
            row.kind = L"sub2api";
            state->rows.push_back(row);
            state->selected = static_cast<int>(state->rows.size()) - 1;
            RefreshListQuiet(state);
            LoadRowIntoControls(state);
            SetFocus(state->nameEdit);
            SendMessageW(state->nameEdit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            SetStatus(state, L"已新增，填写后请点击保存。", kColorMuted);
        }

        void OnDelete(DialogState* state)
        {
            if (state->selected < 0 || state->selected >= static_cast<int>(state->rows.size()))
            {
                return;
            }
            std::wstring name = state->rows[state->selected].name;
            state->rows.erase(state->rows.begin() + state->selected);
            if (state->selected >= static_cast<int>(state->rows.size()))
            {
                state->selected = static_cast<int>(state->rows.size()) - 1;
            }
            RefreshListQuiet(state);
            LoadRowIntoControls(state);
            SetStatus(state, L"已删除“" + name + L"”，请点击保存生效。", kColorMuted);
        }

        void OnSave(DialogState* state)
        {
            CommitControlsToRow(state);
            // Cookie 类型在界面上固定“留空”，落盘也写成空串。
            for (size_t i = 0; i < state->rows.size(); ++i)
            {
                if (IsCookieKind(state->rows[i].kind))
                {
                    state->rows[i].baseUrl.clear();
                }
            }
            std::wstring error;
            bool ok = state->host.saveRows != nullptr && state->host.saveRows(state->rows, error);
            if (ok)
            {
                state->saved = true;
                SetStatus(state, L"已保存，任务栏最多 30 秒内同步。", kColorOk);
                if (state->host.log != nullptr)
                {
                    state->host.log(L"providers.json 已保存，共 "
                        + std::to_wstring(state->rows.size()) + L" 家。");
                }
            }
            else
            {
                SetStatus(state, error.empty() ? L"保存失败。" : error, kColorError);
            }
        }

        // 切换密钥明文：ES_PASSWORD 只在创建时生效，改 GWL_STYLE 不会刷新显示，
        // 必须用 EM_SETPASSWORDCHAR（0 表示不遮挡）。
        void ApplyKeyVisibility(DialogState* state)
        {
            bool show = SendMessageW(state->showKeyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            wchar_t passwordChar = show ? 0 : kPasswordChar;
            SendMessageW(state->keyEdit, EM_SETPASSWORDCHAR,
                static_cast<WPARAM>(passwordChar), 0);
            InvalidateRect(state->keyEdit, nullptr, TRUE);
            // 切换后回到开头显示，避免停在密钥末尾。
            SendMessageW(state->keyEdit, EM_SETSEL, 0, 0);
            SendMessageW(state->keyEdit, EM_SCROLLCARET, 0, 0);
        }

        void OnSelectChanged(DialogState* state)
        {
            if (state->dragging)
            {
                return;
            }
            // 刷新列表时的中间选中态（例如清空列表瞬间回到 -1）不是用户操作，
            // 放过去会把当前表单内容提交到刚新增的那一行上。
            if (state->loading)
            {
                return;
            }
            int index = SelectedIndex(state);
            if (index == state->selected)
            {
                return;
            }
            CommitControlsToRow(state);
            state->selected = index;
            LoadRowIntoControls(state);
            int count = static_cast<int>(state->rows.size());
            bool hasSelection = index >= 0 && index < count;
            EnableWindow(state->deleteButton, hasSelection ? TRUE : FALSE);
            EnableWindow(state->nameEdit, hasSelection ? TRUE : FALSE);
            EnableWindow(state->kindCombo, hasSelection ? TRUE : FALSE);
            EnableWindow(state->urlEdit, hasSelection ? TRUE : FALSE);
            EnableWindow(state->keyEdit, hasSelection ? TRUE : FALSE);
            EnableWindow(state->showKeyCheck, hasSelection ? TRUE : FALSE);
            EnableWindow(state->testButton,
                state->test == nullptr && hasSelection ? TRUE : FALSE);
            // 统一 EnableWindow 会重新放开 Cookie 类型的 Base URL，这里按类型收回去。
            if (hasSelection)
            {
                ApplyUrlFieldMode(state, state->rows[index].kind, true);
            }
        }

        int HitTestTarget(DialogState* state, POINT dialogPoint)
        {
            POINT pt = dialogPoint;
            MapWindowPoints(state->hwnd, state->list, &pt, 1);
            LVHITTESTINFO hit;
            ZeroMemory(&hit, sizeof(hit));
            hit.pt = pt;
            SendMessageW(state->list, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));
            if (hit.iItem < 0)
            {
                return static_cast<int>(state->rows.size());
            }
            RECT itemRect;
            itemRect.left = LVIR_BOUNDS;
            SendMessageW(state->list, LVM_GETITEMRECT,
                static_cast<WPARAM>(hit.iItem), reinterpret_cast<LPARAM>(&itemRect));
            int middle = itemRect.top + (itemRect.bottom - itemRect.top) / 2;
            return hit.iItem + (pt.y > middle ? 1 : 0);
        }

        void ShowInsertMark(DialogState* state, int target)
        {
            LVINSERTMARK mark;
            ZeroMemory(&mark, sizeof(mark));
            mark.cbSize = sizeof(mark);
            if (target <= 0)
            {
                mark.iItem = 0;
                mark.dwFlags = 0;
            }
            else
            {
                mark.iItem = target - 1;
                mark.dwFlags = LVIM_AFTER;
            }
            if (state->rows.empty())
            {
                mark.iItem = -1;
            }
            SendMessageW(state->list, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&mark));
        }

        void ClearInsertMark(DialogState* state)
        {
            LVINSERTMARK mark;
            ZeroMemory(&mark, sizeof(mark));
            mark.cbSize = sizeof(mark);
            mark.iItem = -1;
            SendMessageW(state->list, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&mark));
        }

        void EndDrag(DialogState* state, bool commit)
        {
            if (!state->dragging)
            {
                return;
            }
            state->dragging = false;
            if (GetCapture() == state->hwnd)
            {
                ReleaseCapture();
            }
            ClearInsertMark(state);
            int from = state->dragFrom;
            int to = state->dragTarget;
            state->dragFrom = -1;
            state->dragTarget = -1;
            if (!commit || from < 0 || to < 0
                || from >= static_cast<int>(state->rows.size()))
            {
                return;
            }
            if (to > from)
            {
                --to;
            }
            if (to == from)
            {
                return;
            }
            ProviderRow moved = state->rows[from];
            state->rows.erase(state->rows.begin() + from);
            if (to < 0)
            {
                to = 0;
            }
            if (to > static_cast<int>(state->rows.size()))
            {
                to = static_cast<int>(state->rows.size());
            }
            state->rows.insert(state->rows.begin() + to, moved);
            state->selected = to;
            RefreshListQuiet(state);
            LoadRowIntoControls(state);
            SetStatus(state, L"顺序已调整，保存后生效。", kColorMuted);
        }

        void CreateControls(DialogState* state)
        {

            MakeControl(state, L"STATIC", L"名称", SS_LEFT, 0,
                250, 42, 100, 26, 0, state->font);
            MakeControl(state, L"STATIC", L"类型", SS_LEFT, 0,
                250, 78, 100, 26, 0, state->font);
            MakeControl(state, L"STATIC", L"Base URL", SS_LEFT, 0,
                250, 114, 100, 26, 0, state->font);
            MakeControl(state, L"STATIC", L"Key/Cookie", SS_LEFT, 0,
                250, 150, 100, 26, 0, state->font);

            MakeControl(state, L"STATIC", L"1. 一次一家：余额 / 今日消费。",
                SS_LEFT, 0, 250, 256, 346, 24, 0, state->font);
            MakeControl(state, L"STATIC", L"2. 15 分钟内变化，多家 30 秒轮换。",
                SS_LEFT, 0, 250, 276, 346, 24, 0, state->font);
            MakeControl(state, L"STATIC", L"3. 今日优先官方，缺失用余额差估算。",
                SS_LEFT, 0, 250, 296, 346, 24, 0, state->font);

            state->taskbarLabel = MakeControl(state, L"STATIC", L"任务栏", SS_CENTER, 0,
                248, 335, 52, 20, IDC_HINT, state->hintFont);
            state->sampleLabel = MakeControl(state, L"STATIC", L"示例", SS_CENTER, 0,
                248, 355, 52, 20, IDC_HINT, state->hintFont);

            MakeControl(state, L"STATIC", L"拖动条目可调整顺序", SS_LEFT, 0,
                22, 302, 194, 20, IDC_HINT, state->hintFont);

            state->list = MakeControl(state, L"SysListView32", L"",
                WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL
                    | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                WS_EX_CLIENTEDGE, 22, 42, 194, 254, IDC_LIST, state->font);
            SendMessageW(state->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            LVCOLUMNW column;
            ZeroMemory(&column, sizeof(column));
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(L"名称");
            column.cx = Scaled(state, 186);
            column.iSubItem = 0;
            SendMessageW(state->list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));

            state->nameEdit = MakeControl(state, L"EDIT", L"",
                WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                356, 38, 238, 30, IDC_NAME, state->font);
            state->kindCombo = MakeControl(state, L"COMBOBOX", L"",
                WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0,
                356, 74, 238, 220, IDC_KIND, state->font);
            state->urlEdit = MakeControl(state, L"EDIT", L"",
                WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                356, 110, 238, 30, IDC_URL, state->font);
            state->keyEdit = MakeControl(state, L"EDIT", L"",
                WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, WS_EX_CLIENTEDGE,
                356, 146, 180, 30, IDC_KEY, state->font);
            state->showKeyCheck = MakeControl(state, L"BUTTON", L"显示",
                WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                540, 150, 54, 26, IDC_SHOW_KEY, state->hintFont);
            SendMessageW(state->keyEdit, EM_SETPASSWORDCHAR,
                static_cast<WPARAM>(kPasswordChar), 0);

            // 单行 + 省略号：SS_LEFT 的 STATIC 会自动折行，长错误信息会压到下面的
            // “轮换规则”分组框上；加了 SS_ENDELLIPSIS 后超长内容收成一行。
            state->status = MakeControl(state, L"STATIC", L"",
                SS_LEFT | SS_ENDELLIPSIS, 0,
                250, 191, 344, 26, IDC_STATUS, state->hintFont);

            state->sample = MakeControl(state, L"STATIC", L"", SS_OWNERDRAW, 0,
                304, 325, 292, 50, IDC_SAMPLE, nullptr);

            state->addButton = MakeControl(state, L"BUTTON", L"新增",
                WS_TABSTOP | BS_PUSHBUTTON, 0, 22, 330, 94, 34, IDC_ADD, state->font);
            state->deleteButton = MakeControl(state, L"BUTTON", L"删除",
                WS_TABSTOP | BS_PUSHBUTTON, 0, 122, 330, 94, 34, IDC_DELETE, state->font);

            state->testButton = MakeControl(state, L"BUTTON", L"测试连接",
                WS_TABSTOP | BS_PUSHBUTTON, 0, 157, 398, 110, 34, IDC_TEST, state->font);
            state->saveButton = MakeControl(state, L"BUTTON", L"保存",
                WS_TABSTOP | BS_PUSHBUTTON, 0, 277, 398, 88, 34, IDC_SAVE, state->font);
            MakeControl(state, L"BUTTON", L"关闭",
                WS_TABSTOP | BS_PUSHBUTTON, 0, 375, 398, 88, 34, IDC_CLOSE, state->font);

            for (int i = 0; i < kKindCount; ++i)
            {
                SendMessageW(state->kindCombo, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(kKinds[i].label));
            }
            SendMessageW(state->kindCombo, CB_SETCURSEL, 0, 0);
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            DialogState* state = StateOf(hwnd);
            if (message == WM_NCCREATE)
            {
                CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                state = reinterpret_cast<DialogState*>(create->lpCreateParams);
                state->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(state));
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            if (state == nullptr)
            {
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }

            switch (message)
            {
            case WM_CREATE:
            {
                UINT value = GetDpiForWindow(hwnd);
                if (value >= 96 && value <= 480)
                {
                    state->dpi = value;
                }
                state->font = MakeFontDip(state->dpi, 18, FW_NORMAL, L"Times New Roman", false);
                state->hintFont = MakeFontDip(state->dpi, 13, FW_NORMAL, L"Times New Roman", false);

                state->style = TaskbarStyle();
                if (state->host.taskbarStyle != nullptr)
                {
                    state->host.taskbarStyle(state->style);
                }
                state->sampleFont = MakeFont(state->dpi, state->style.sizePt + 1,
                    state->style.bold ? FW_BOLD : FW_NORMAL,
                    state->style.fontName[0] != L'\0' ? state->style.fontName : L"Microsoft YaHei",
                    state->style.italic);

                CreateControls(state);

                state->rows = state->host.loadRows != nullptr
                    ? state->host.loadRows() : std::vector<ProviderRow>();
                state->selected = state->rows.empty() ? -1 : 0;
                RefreshListQuiet(state);
                LoadRowIntoControls(state);
                if (state->rows.empty())
                {
                    SetStatus(state, L"当前没有供应商，请点击“新增”。", kColorMuted);
                }
                // 没有 WS_MINIMIZEBOX 时，系统菜单里的“最小化”其实仍然可用；
                // 一旦最小化，这个窗口没有任务栏按钮，就再也点不回来了。
                EnableMenuItem(GetSystemMenu(hwnd, FALSE), SC_MINIMIZE,
                    MF_BYCOMMAND | MF_GRAYED);
                return 0;
            }
            case WM_COMMAND:
            {
                int id = static_cast<int>(LOWORD(wParam));
                int code = static_cast<int>(HIWORD(wParam));
                switch (id)
                {
                case IDC_ADD:
                    OnNew(state);
                    return 0;
                case IDC_DELETE:
                    OnDelete(state);
                    return 0;
                case IDC_TEST:
                    StartTest(state);
                    return 0;
                case IDC_SAVE:
                    OnSave(state);
                    return 0;
                case IDC_CLOSE:
                    DestroyWindow(state->hwnd);
                    return 0;
                case IDC_SHOW_KEY:
                    ApplyKeyVisibility(state);
                    return 0;
                case IDC_KIND:
                    if (code == CBN_SELCHANGE && !state->loading)
                    {
                        CommitControlsToRow(state);
                        UpdateSelectedTitle(state);
                        if (state->selected >= 0
                            && state->selected < static_cast<int>(state->rows.size()))
                        {
                            const ProviderRow& row = state->rows[state->selected];
                            // 切换类型时先摘掉占位/恢复地址，期间不要让
                            // EN_CHANGE 把中间态提交回行里。
                            bool wasLoading = state->loading;
                            state->loading = true;
                            ApplyUrlFieldMode(state, row.kind, true);
                            if (!IsCookieKind(row.kind))
                            {
                                // 从 Cookie 类型切回来时恢复这一行原来的地址。
                                SetWindowTextW(state->urlEdit, row.baseUrl.c_str());
                            }
                            state->loading = wasLoading;
                            SetStatus(state, KeyHint(row.kind, !row.key.empty()),
                                kColorMuted);
                        }
                    }
                    return 0;
                case IDC_NAME:
                    if (code == EN_CHANGE && !state->loading)
                    {
                        CommitControlsToRow(state);
                        UpdateSelectedTitle(state);
                    }
                    return 0;
                case IDC_URL:
                case IDC_KEY:
                    if (code == EN_CHANGE && !state->loading)
                    {
                        CommitControlsToRow(state);
                    }
                    return 0;
                default:
                    return DefWindowProcW(hwnd, message, wParam, lParam);
                }
            }
            case WM_NOTIFY:
            {
                NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
                if (header == nullptr || header->hwndFrom != state->list)
                {
                    break;
                }
                if (header->code == LVN_ITEMCHANGED)
                {
                    NMLISTVIEW* changed = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if ((changed->uNewState & LVIS_SELECTED) != 0
                        && (changed->uOldState & LVIS_SELECTED) == 0)
                    {
                        OnSelectChanged(state);
                    }
                }
                else if (header->code == LVN_BEGINDRAG)
                {
                    state->dragging = true;
                    state->dragFrom = SelectedIndex(state);
                    state->dragTarget = state->dragFrom;
                    if (state->dragFrom >= 0)
                    {
                        SetCapture(state->hwnd);
                    }
                }
                return 0;
            }
            case WM_MOUSEMOVE:
                if (state->dragging)
                {
                    POINT pt;
                    pt.x = GET_X_LPARAM(lParam);
                    pt.y = GET_Y_LPARAM(lParam);
                    state->dragTarget = HitTestTarget(state, pt);
                    ShowInsertMark(state, state->dragTarget);
                }
                return 0;
            case WM_LBUTTONUP:
                if (state->dragging)
                {
                    EndDrag(state, true);
                    return 0;
                }
                break;
            case WM_CAPTURECHANGED:
                if (state->dragging)
                {
                    EndDrag(state, false);
                }
                return 0;
            case WM_TIMER:
                if (wParam == kTestTimer)
                {
                    PollTest(state);
                }
                return 0;
            case WM_DRAWITEM:
            {
                DRAWITEMSTRUCT* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (item != nullptr && item->CtlID == IDC_SAMPLE)
                {
                    DrawSample(state, item->hDC, item->rcItem);
                    return TRUE;
                }
                break;
            }
            case WM_CTLCOLORSTATIC:
            {
                HDC dc = reinterpret_cast<HDC>(wParam);
                HWND control = reinterpret_cast<HWND>(lParam);
                int id = GetDlgCtrlID(control);
                SetBkMode(dc, TRANSPARENT);
                if (id == IDC_STATUS)
                {
                    SetTextColor(dc, state->statusColor);
                }
                else if (id == IDC_HINT)
                {
                    SetTextColor(dc, kColorMuted);
                }
                else
                {
                    SetTextColor(dc, kColorText);
                }
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
                HGDIOBJ oldPen = SelectObject(dc, pen);
                const int frames[3][4] = {
                    { 12, 18, 214, 366 },
                    { 238, 18, 370, 204 },
                    { 238, 240, 370, 144 }
                };
                const wchar_t* captions[3] = {
                    L"供应商列表", L"供应商信息", L"轮换规则"
                };
                for (int i = 0; i < 3; ++i)
                {
                    int x = Scaled(state, frames[i][0]);
                    int y = Scaled(state, frames[i][1]);
                    int w = Scaled(state, frames[i][2]);
                    int h = Scaled(state, frames[i][3]);
                    SIZE captionSize = { 0, 0 };
                    HGDIOBJ oldFont = SelectObject(dc, state->font);
                    GetTextExtentPoint32W(dc, captions[i], lstrlenW(captions[i]),
                        &captionSize);
                    SelectObject(dc, oldFont);
                    int gapStart = x + Scaled(state, 10);
                    int gapEnd = gapStart + captionSize.cx + Scaled(state, 4);
                    MoveToEx(dc, x, y, nullptr);
                    LineTo(dc, gapStart, y);
                    MoveToEx(dc, gapEnd, y, nullptr);
                    LineTo(dc, x + w, y);
                    MoveToEx(dc, x, y, nullptr);
                    LineTo(dc, x, y + h);
                    MoveToEx(dc, x + w - 1, y, nullptr);
                    LineTo(dc, x + w - 1, y + h);
                    MoveToEx(dc, x, y + h - 1, nullptr);
                    LineTo(dc, x + w, y + h - 1);
                }
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, kColorText);
                HGDIOBJ captionFont = SelectObject(dc, state->font);
                for (int i = 0; i < 3; ++i)
                {
                    int x = Scaled(state, frames[i][0]);
                    int y = Scaled(state, frames[i][1]);
                    SIZE captionSize = { 0, 0 };
                    GetTextExtentPoint32W(dc, captions[i], lstrlenW(captions[i]),
                        &captionSize);
                    RECT captionRect = {
                        x + Scaled(state, 10), y - captionSize.cy / 2,
                        x + Scaled(state, 10) + captionSize.cx + Scaled(state, 8),
                        y + captionSize.cy / 2
                    };
                    DrawTextW(dc, captions[i], -1, &captionRect,
                        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
                }
                SelectObject(dc, captionFont);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_SYSCOMMAND:
                // 双保险：即使“最小化”从别的路径触发也不让它生效。
                if ((wParam & 0xFFF0) == SC_MINIMIZE)
                {
                    return 0;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(state->hwnd);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd, kTestTimer);
                if (state->test != nullptr)
                {
                    ReleaseReference(state->test);
                    state->test = nullptr;
                }
                if (state->font != nullptr)
                {
                    DeleteObject(state->font);
                    state->font = nullptr;
                }
                if (state->hintFont != nullptr)
                {
                    DeleteObject(state->hintFont);
                    state->hintFont = nullptr;
                }
                if (state->sampleFont != nullptr)
                {
                    DeleteObject(state->sampleFont);
                    state->sampleFont = nullptr;
                }
                state->hwnd = nullptr;
                state->destroyed = true;
                // 用线程消息唤醒嵌套消息循环，避免 PostMessage 到已销毁窗口被丢弃。
                PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
                return 0;
            default:
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        bool RegisterWindowClass(HINSTANCE instance)
        {
            WNDCLASSEXW wc;
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
            wc.lpszClassName = kWindowClass;
            wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
            wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
            if (RegisterClassExW(&wc) != 0)
            {
                return true;
            }
            return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }

        void CenterOnParent(HWND hwnd, HWND parent)
        {
            // 以父窗口所在显示器的工作区居中，而不是按父窗口矩形居中：
            // 双击入口的父窗口是任务栏窗口，贴着屏幕底边，按它居中会把
            // 对话框整个推到屏幕外面（2026-10-20 实测踩到）。
            HMONITOR monitor = MonitorFromWindow(
                parent != nullptr ? parent : hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info;
            info.cbSize = sizeof(info);
            RECT area;
            if (GetMonitorInfoW(monitor, &info))
            {
                area = info.rcWork;
            }
            else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0))
            {
                return;
            }

            RECT self;
            if (!GetWindowRect(hwnd, &self))
            {
                return;
            }
            int width = self.right - self.left;
            int height = self.bottom - self.top;
            int x = area.left + ((area.right - area.left) - width) / 2;
            int y = area.top + ((area.bottom - area.top) - height) / 2;
            if (x + width > area.right)
            {
                x = area.right - width;
            }
            if (y + height > area.bottom)
            {
                y = area.bottom - height;
            }
            if (x < area.left)
            {
                x = area.left;
            }
            if (y < area.top)
            {
                y = area.top;
            }
            SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
        }

        typedef BOOL(WINAPI* AdjustWindowRectExForDpiFunc)(LPRECT, DWORD, BOOL, DWORD, UINT);
    }

    bool ShowOptionsDialog(void* hParent, const OptionsHost& host)
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ShowOptionsDialog), &module);
        HINSTANCE instance = module;
        if (!RegisterWindowClass(instance))
        {
            return false;
        }

        INITCOMMONCONTROLSEX controls;
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&controls);

        DialogState* state = new (std::nothrow) DialogState();
        if (state == nullptr)
        {
            return false;
        }
        state->instance = instance;
        state->host = host;

        HWND parent = static_cast<HWND>(hParent);
        UINT dpi = GetDpiForSystem();
        if (host.hostDpi != nullptr)
        {
            int hostValue = host.hostDpi();
            if (hostValue >= 96 && hostValue <= 480)
            {
                dpi = static_cast<UINT>(hostValue);
            }
        }
        if (dpi < 96 || dpi > 480)
        {
            dpi = 96;
        }
        if (parent != nullptr && IsWindow(parent))
        {
            UINT value = GetDpiForWindow(parent);
            if (value >= 96 && value <= 480)
            {
                dpi = value;
            }
        }
        state->dpi = dpi;
        RECT client = { 0, 0, Scaled(state, kClientWidth), Scaled(state, kClientHeight) };
        // 故意不给 WS_MINIMIZEBOX：属主是 TrafficMonitor 的工具窗口，
        // 最小化后任务栏不会给出按钮，窗口就再也点不回来了。
        DWORD style = WS_CAPTION | WS_SYSMENU;
        DWORD exStyle = WS_EX_DLGMODALFRAME;
        AdjustWindowRectExForDpiFunc adjust = reinterpret_cast<AdjustWindowRectExForDpiFunc>(
            reinterpret_cast<void*>(GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")));
        if (adjust != nullptr)
        {
            adjust(&client, style, FALSE, exStyle, dpi);
        }
        else
        {
            AdjustWindowRectEx(&client, style, FALSE, exStyle);
        }

        if (parent != nullptr && IsWindow(parent))
        {
            EnableWindow(parent, FALSE);
        }
        HWND hwnd = CreateWindowExW(exStyle, kWindowClass, kWindowTitle, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            client.right - client.left, client.bottom - client.top,
            parent, nullptr, instance, state);
        if (hwnd == nullptr)
        {
            if (parent != nullptr && IsWindow(parent))
            {
                EnableWindow(parent, TRUE);
            }
            delete state;
            return false;
        }
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,
            reinterpret_cast<LPARAM>(LoadIconW(instance, MAKEINTRESOURCEW(1))));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
            reinterpret_cast<LPARAM>(LoadIconW(instance, MAKEINTRESOURCEW(1))));
        CenterOnParent(hwnd, parent);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SendMessageW(hwnd, WM_NEXTDLGCTL,
            reinterpret_cast<WPARAM>(state->list), TRUE);

        while (!state->destroyed)
        {
            MSG message;
            BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got == 0)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            if (got == -1)
            {
                break;
            }
            if (state->destroyed)
            {
                break;
            }
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE
                && message.hwnd != nullptr && GetAncestor(message.hwnd, GA_ROOT) == hwnd)
            {
                DestroyWindow(hwnd);
                continue;
            }
            if (!IsDialogMessageW(hwnd, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        bool changed = state->saved;
        if (IsWindow(hwnd))
        {
            DestroyWindow(hwnd);
        }
        if (parent != nullptr && IsWindow(parent))
        {
            EnableWindow(parent, TRUE);
            SetForegroundWindow(parent);
        }
        delete state;
        return changed;
    }

    namespace
    {
        struct FindDialogData
        {
            DWORD pid;
            HWND hwnd;
        };

        BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM lParam)
        {
            FindDialogData* data = reinterpret_cast<FindDialogData*>(lParam);
            wchar_t className[64] = L"";
            GetClassNameW(hwnd, className, 64);
            if (wcscmp(className, kWindowClass) != 0)
            {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == data->pid)
            {
                data->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        }
    }

    bool RaiseOpenDialog()
    {
        // 只认本进程的窗口：同时跑多个 TrafficMonitor 时，别人的同名窗口既点不动也不该去点。
        FindDialogData data = { GetCurrentProcessId(), nullptr };
        EnumThreadWindows(GetCurrentThreadId(), FindDialogProc,
            reinterpret_cast<LPARAM>(&data));
        if (data.hwnd == nullptr)
        {
            EnumWindows(FindDialogProc, reinterpret_cast<LPARAM>(&data));
        }
        if (data.hwnd == nullptr)
        {
            return false;
        }
        if (IsIconic(data.hwnd))
        {
            ShowWindow(data.hwnd, SW_RESTORE);
        }
        SetForegroundWindow(data.hwnd);
        return true;
    }
}
