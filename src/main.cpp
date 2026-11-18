#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>
#include "history.hpp"
#include "clipboard.hpp"
#include "resource.h"

namespace fs = std::filesystem;
constexpr wchar_t ClassName[] = L"ClipboardHistoryCpp";
constexpr wchar_t AppName[] = L"粘贴板收纳盒";
constexpr UINT TrayMessage = WM_APP + 1;
constexpr COLORREF Background = RGB(245, 247, 250);
constexpr COLORREF Ink = RGB(23, 35, 52);
constexpr COLORREF Accent = RGB(0, 105, 217);
enum Control { Search = 100, GroupList, GroupMenu, Move, Items, Preview, Copy, Paste,
               Erase, Clear, Connect, Fold, Pin, TrayOpen, TrayExit };

std::wstring windowText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    std::wstring result(len + 1, L'\0');
    result.resize(GetWindowTextW(hwnd, result.data(), len + 1));
    return result;
}

struct NameDialog {
    std::wstring value;
    HWND edit = nullptr;
    static INT_PTR CALLBACK proc(HWND window, UINT message, WPARAM key, LPARAM param) {
        auto self = reinterpret_cast<NameDialog*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<NameDialog*>(param);
            SetWindowLongPtrW(window, DWLP_USER, param);
            SetWindowTextW(window, L"分组名称");
            HWND parent = GetParent(window);
            RECT parentRect{};
            GetWindowRect(parent, &parentRect);
            SetWindowPos(window, HWND_TOPMOST, parentRect.left + 30, parentRect.top + 50, 0, 0, SWP_NOSIZE);
            auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            auto make = [&](const wchar_t* cls, const wchar_t* label, DWORD style, int x, int y, int w, int h, int id) {
                RECT r{x, y, x + w, y + h};
                MapDialogRect(window, &r);
                auto control = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style,
                    r.left, r.top, r.right-r.left, r.bottom-r.top, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                return control;
            };
            self->edit = make(L"EDIT", self->value.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 10, 10, 195, 16, 120);
            make(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 91, 35, 54, 16, IDOK);
            make(L"BUTTON", L"取消", WS_TABSTOP, 151, 35, 54, 16, IDCANCEL);
            SendMessageW(self->edit, EM_SETLIMITTEXT, 60, 0);
            SetFocus(self->edit);
            SendMessageW(self->edit, EM_SETSEL, 0, -1);
            return FALSE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(key) == IDCANCEL)) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(key) == IDOK) {
            self->value = windowText(self->edit);
            auto first = self->value.find_first_not_of(L" \t　");
            auto last = self->value.find_last_not_of(L" \t　");
            if (first == std::wstring::npos) return TRUE;
            self->value = self->value.substr(first, last - first + 1);
            if (self->value == L"全部记录" || self->value == L"未分组") {
                MessageBoxW(window, L"请使用其他名称。", L"名称已保留", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            EndDialog(window, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    std::optional<std::wstring> ask(HWND owner) {
        struct alignas(DWORD) Template {
            DLGTEMPLATE dialog;
            WORD menu, cls, title;
        } layout{};
        layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
        layout.dialog.cx = 215;
        layout.dialog.cy = 61;
        if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner, proc,
                                    reinterpret_cast<LPARAM>(this)) == IDOK) return value;
        return std::nullopt;
    }
};

class App {
    History history;
    ClipboardMonitor monitor;
    HWND window = nullptr, status = nullptr, group = nullptr, search = nullptr, items = nullptr, preview = nullptr;
    struct Target {
        HWND window = nullptr;
        HWND focus = nullptr;
        DWORD pid = 0;
        DWORD thread = 0;
    };
    struct PasteRequest {
        Target target;
        std::wstring text;
        ULONGLONG deadline;
    };
    Target recordedTarget;
    std::optional<PasteRequest> pendingPaste;
    HWND pressedButton = nullptr;
    DWORD lastSequence = 0;
    HFONT font = nullptr;
    HBRUSH background = CreateSolidBrush(Background), paper = CreateSolidBrush(RGB(255,255,255));
    std::vector<Group> allGroups;
    std::vector<Record> rows;
    std::deque<ClipboardMonitor::Entry> pending;
    bool connected = true, folded = false, moving = false, tray = false, listener = false, hotkey = false;
    unsigned generation = 0;
    int dpi = 96, normalHeight = 535, normalWidth = 480;
    NOTIFYICONDATAW icon{};
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    int px(int value) const { return MulDiv(value, dpi, 96); }
    HWND control(const wchar_t* type, const wchar_t* label, int id, DWORD style, DWORD ex = 0) {
        auto child = CreateWindowExW(ex, type, label, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        if (!child) throw std::runtime_error("create control");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return child;
    }
    void place(int id, int x, int y, int w, int h) const {
        MoveWindow(GetDlgItem(window, id), x, y, std::max(1,w), std::max(1,h), TRUE);
    }
    void statusText(const wchar_t* text = nullptr) {
        SetWindowTextW(status, text ? text : !listener ? L"剪贴板监听不可用" : !connected ? L"已断开"
            : !hotkey ? L"已连接 · 快捷键被占用" : L"已连接");
    }
    Id groupId() const {
        auto index = SendMessageW(group, CB_GETCURSEL, 0, 0);
        return index < 0 ? -1 : static_cast<Id>(SendMessageW(group, CB_GETITEMDATA, index, 0));
    }
    void sizeHistoryColumns() {
        RECT area{};
        GetClientRect(items, &area);
        const int width = static_cast<int>(area.right);
        const int timeWidth = px(143);
        const int groupWidth = groupId() == -1 ? std::clamp(width / 4, px(80), px(170)) : 0;
        ListView_SetColumnWidth(items, 0, timeWidth);
        ListView_SetColumnWidth(items, 1, groupWidth);
        ListView_SetColumnWidth(items, 2, std::max(px(55), width - timeWidth - groupWidth - px(4)));
    }
    int selection() const { return ListView_GetNextItem(items, -1, LVNI_SELECTED); }
    void fillGroups(Id selected = -1) {
        allGroups = history.groups();
        SendMessageW(group, CB_RESETCONTENT, 0, 0);
        auto add = [&](Id id, const wchar_t* label) {
            auto index = SendMessageW(group, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(group, CB_SETITEMDATA, index, static_cast<LPARAM>(id));
            if (id == selected) SendMessageW(group, CB_SETCURSEL, index, 0);
        };
        add(-1, L"全部记录");
        add(0, L"未分组");
        for (const auto& item : allGroups) add(item.id, item.name.c_str());
        if (SendMessageW(group, CB_GETCURSEL, 0, 0) < 0) SendMessageW(group, CB_SETCURSEL, 0, 0);
        sizeHistoryColumns();
    }
    void refresh() {
        int old = selection();
        Id keep = old >= 0 && static_cast<size_t>(old) < rows.size() ? rows[old].id : 0;
        auto newRows = history.records(groupId(), windowText(search));
        SendMessageW(items, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(items);
        rows = std::move(newRows);
        int restore = -1;
        for (size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i];
            std::wstring part = row.text.substr(0, 160);
            std::replace(part.begin(), part.end(), L'\n', L' ');
            std::replace(part.begin(), part.end(), L'\r', L' ');
            LVITEMW info{};
            info.mask = LVIF_TEXT;
            info.iItem = static_cast<int>(i);
            info.pszText = row.time.data();
            ListView_InsertItem(items, &info);
            auto groupLabel = row.groupName.empty() ? std::wstring(L"未分组") : row.groupName;
            ListView_SetItemText(items, info.iItem, 1, groupLabel.data());
            ListView_SetItemText(items, info.iItem, 2, part.data());
            if (row.id == keep) restore = info.iItem;
        }
        if (restore >= 0) {
            ListView_SetItemState(items, restore, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        } else {
            SetWindowTextW(preview, L"");
        }
        SendMessageW(items, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(items, nullptr, TRUE);
    }

    static HWND focusedControl(DWORD thread) {
        GUITHREADINFO info{sizeof(info)};
        return thread && GetGUIThreadInfo(thread, &info) ? info.hwndFocus : nullptr;
    }

    void rememberTarget() {
        HWND current = GetForegroundWindow();
        DWORD pid = 0;
        DWORD thread = GetWindowThreadProcessId(current, &pid);
        if (pid && pid != GetCurrentProcessId())
            recordedTarget = {current, focusedControl(thread), pid, thread};
    }

    void finishPaste(const wchar_t* message = nullptr) {
        KillTimer(window, 2);
        pendingPaste.reset();
        statusText(message);
    }

    void attemptPaste() {
        if (!pendingPaste) return;
        const auto& request = *pendingPaste;
        const auto& target = request.target;
        DWORD pid = 0;
        DWORD thread = GetWindowThreadProcessId(target.window, &pid);
        if (!thread || thread != target.thread || pid != target.pid || IsIconic(target.window)) {
            finishPaste(L"请先点击目标输入位置");
            return;
        }

        bool expired = GetTickCount64() >= request.deadline;
        auto foreground = GetForegroundWindow();
        if (foreground != target.window) {
            DWORD activePid = 0;
            GetWindowThreadProcessId(foreground, &activePid);
            if (activePid && activePid != GetCurrentProcessId()) {
                finishPaste(L"目标窗口已切换，粘贴已取消");
            } else if (expired) {
                finishPaste(L"无法切回目标窗口，请点一下原光标");
            }
            return;
        }

        if (target.focus && focusedControl(thread) != target.focus) {
            if (expired) finishPaste(L"输入焦点已变化，请点一下原光标");
            return;
        }

        for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN}) {
            if (GetAsyncKeyState(key) & 0x8000) {
                if (expired) finishPaste(L"请松开辅助键后重试");
                return;
            }
        }

        if (!ClipboardMonitor::write(window, request.text, lastSequence)) {
            if (expired) finishPaste(L"剪贴板被占用，请重试");
            return;
        }

        if (GetForegroundWindow() != target.window
            || (target.focus && focusedControl(thread) != target.focus)
            || GetClipboardOwner() != window
            || GetClipboardSequenceNumber() != lastSequence) {
            finishPaste(L"目标或剪贴板已变化，粘贴已取消");
            return;
        }

        // Standard Win32 edit boxes (including rename fields) accept WM_PASTE
        // directly, without depending on simulated Ctrl+V delivery.
        wchar_t focusClass[32]{};
        if (target.focus && GetClassNameW(target.focus, focusClass, ARRAYSIZE(focusClass))
            && lstrcmpiW(focusClass, L"Edit") == 0) {
            if (PostMessageW(target.focus, WM_PASTE, 0, 0)) {
                finishPaste(L"已发送粘贴");
                return;
            }
        }

        INPUT keys[4]{};
        const WORD codes[] = {VK_CONTROL, 'V', 'V', VK_CONTROL};
        for (int i = 0; i < 4; ++i) {
            keys[i].type = INPUT_KEYBOARD;
            keys[i].ki.wVk = codes[i];
            if (i >= 2) keys[i].ki.dwFlags = KEYEVENTF_KEYUP;
        }
        finishPaste(SendInput(4, keys, sizeof(INPUT)) == 4 ? L"已发送粘贴" : L"自动粘贴被拦截，可手动按 Ctrl+V");
    }

    void copyOrPaste(bool paste) {
        int selected = selection();
        if (selected < 0 || static_cast<size_t>(selected) >= rows.size()) {
            statusText(L"请先选择历史记录");
            return;
        }
        auto text = SendMessageW(preview, EM_GETMODIFY, 0, 0) ? windowText(preview) : rows[selected].text;
        if (text.empty()) return;
        if (pendingPaste) finishPaste();
        if (!paste) {
            statusText(ClipboardMonitor::write(window, text, lastSequence)
                ? L"已复制 · 在目标位置按 Ctrl+V" : L"剪贴板被占用，请重试");
            return;
        }

        rememberTarget();
        if (!recordedTarget.window) {
            statusText(L"请先点击目标输入位置");
            return;
        }
        pendingPaste = PasteRequest{recordedTarget, std::move(text), GetTickCount64() + 450};
        if (GetForegroundWindow() != recordedTarget.window)
            SetForegroundWindow(recordedTarget.window);
        if (!SetTimer(window, 2, 25, nullptr)) {
            finishPaste(L"无法开始粘贴，请重试");
            return;
        }
        attemptPaste();
    }

    static LRESULT CALLBACK mouseControl(HWND child, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<App*>(data);
        if (msg == WM_MOUSEACTIVATE) { self.rememberTarget(); return MA_NOACTIVATE; }
        if (id == Items && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
            self.rememberTarget();
            LVHITTESTINFO hit{};
            hit.pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            int selected = ListView_HitTest(child, &hit);
            if (selected >= 0) {
                ListView_SetItemState(child, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                if (msg == WM_LBUTTONDBLCLK) self.copyOrPaste(true);
            }
            return 0;
        }
        if ((id == Paste || id == Copy) && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
            self.rememberTarget();
            self.pressedButton = child;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, child, 0};
            TrackMouseEvent(&tracking);
            SendMessageW(child, BM_SETSTATE, TRUE, 0);
            if (id == Paste) self.copyOrPaste(true);
            return 0;
        }
        if ((id == Paste || id == Copy) && msg == WM_LBUTTONUP) {
            bool armed = self.pressedButton == child;
            self.pressedButton = nullptr;
            SendMessageW(child, BM_SETSTATE, FALSE, 0);
            RECT rect{}; GetClientRect(child, &rect);
            POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (id == Copy && armed && PtInRect(&rect, point)) self.copyOrPaste(false);
            return 0;
        }
        if ((id == Paste || id == Copy) &&
            (msg == WM_CANCELMODE || msg == WM_MOUSELEAVE)) {
            if (self.pressedButton == child) self.pressedButton = nullptr;
            SendMessageW(child, BM_SETSTATE, FALSE, 0);
        }
        return DefSubclassProc(child, msg, wp, lp);
    }
    void groupActions(int command) {
        int selected = selection();
        if (command == Move && selected < 0) {
            statusText(L"请先选择要移动的记录");
            return;
        }
        Id item = selected < 0 ? 0 : rows[selected].id;
        Id id = groupId();
        HMENU menu = CreatePopupMenu();
        if (command == Move) {
            AppendMenuW(menu, MF_STRING, 1, L"未分组");
            for (size_t i = 0; i < allGroups.size(); ++i)
                AppendMenuW(menu, MF_STRING, i+2, allGroups[i].name.c_str());
        } else {
            AppendMenuW(menu, MF_STRING, 1, L"新建分组");
            AppendMenuW(menu, MF_STRING | (id > 0 ? 0 : MF_GRAYED), 2, L"重命名分组");
            AppendMenuW(menu, MF_STRING | (id > 0 ? 0 : MF_GRAYED), 3, L"删除分组");
        }
        RECT rect{};
        GetWindowRect(GetDlgItem(window, command), &rect);
        UINT picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, rect.left, rect.bottom, 0, window, nullptr);
        DestroyMenu(menu);
        if (!picked) return;
        if (command == Move) {
            history.move(item, picked == 1 ? 0 : allGroups.at(picked-2).id);
        } else if (picked == 3) {
            if (MessageBoxW(window, L"删除分组后，组内记录会移回未分组。确定继续吗？", L"删除分组",
                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
            history.removeGroup(id);
            fillGroups();
        } else {
            NameDialog dialog;
            if (picked == 2) {
                for (const auto& g : allGroups) if (g.id == id) dialog.value = g.name;
            }
            if (auto name = dialog.ask(window)) {
                history.nameGroup(picked == 1 ? 0 : id, *name);
                fillGroups(picked == 1 ? -1 : id);
            }
        }
        refresh();
    }

    void layout() {
        RECT r{}; GetClientRect(window, &r);
        const int w = r.right, h = r.bottom, m = px(14), gap = px(7);
        MoveWindow(status, m, px(12), w - 2*m - px(91), px(24), TRUE);
        place(Fold, w-m-px(82), px(7), px(82), px(30));
        if (folded) return;
        place(Pin, w-m-px(58), px(48), px(58), px(28));
        place(Search, m, px(48), w-2*m-px(65), px(28));
        place(Move, w-m-px(66), px(85), px(66), px(28));
        place(GroupMenu, w-m-px(155), px(85), px(82), px(28));
        place(GroupList, m, px(85), w-2*m-px(163), px(205));
        place(Clear, w-m-px(82), px(122), px(82), px(28));
        place(Erase, w-m-px(171), px(122), px(82), px(28));
        int footer = h-px(46);
        int previewHeight = std::min(px(108), (h-px(214))/3);
        int previewTop = footer-previewHeight-gap;
        place(Items, m, px(158), w-2*m, previewTop-gap-px(158));
        sizeHistoryColumns();
        place(Preview, m, previewTop, w-2*m, previewHeight);
        place(Connect, m, footer, px(72), px(32));
        place(Paste, w-m-px(103), footer, px(103), px(32));
        place(Copy, w-m-px(214), footer, px(104), px(32));
    }

    int compactHeight() const {
        RECT compact{0, 0, px(400), px(44)};
        AdjustWindowRectEx(&compact, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOPMOST);
        return compact.bottom-compact.top;
    }

    void fold() {
        if (!folded && IsZoomed(window)) ShowWindow(window, SW_RESTORE);
        RECT r{}; GetWindowRect(window, &r);
        if (!folded) {
            normalHeight = r.bottom-r.top;
            normalWidth = r.right-r.left;
        }
        folded = !folded;
        for (int child : {Search, GroupList, GroupMenu, Move, Items, Preview, Copy, Paste, Erase, Clear, Connect, Pin})
            ShowWindow(GetDlgItem(window, child), folded ? SW_HIDE : SW_SHOWNA);
        SetWindowTextW(GetDlgItem(window, Fold), folded ? L"展开 ▼" : L"收起 ▲");
        SetWindowPos(window, nullptr, 0, 0, normalWidth, folded ? compactHeight() : normalHeight,
                     SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        layout();
    }

    void trayIcon() {
        if (tray) return;
        tray = Shell_NotifyIconW(NIM_ADD, &icon);
    }
    void show() {
        rememberTarget();
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        else ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
    }
    void capture() {
        auto sequence = GetClipboardSequenceNumber();
        if (!connected || sequence == lastSequence) return;
        lastSequence = sequence;
        if (GetClipboardOwner() != window) monitor.notify(sequence, generation, groupId());
    }
    void flush() {
        if (moving) return;
        auto incoming = monitor.drain();
        for (auto& entry : incoming)
            if (entry.generation == generation && connected) pending.push_back(std::move(entry));
        if (pending.empty()) return;
        unsigned count = 0;
        history.run("BEGIN IMMEDIATE");
        try {
            for (const auto& entry : pending) {
                history.add(entry.text, entry.group);
                if (++count == 64) break;
            }
            history.run("COMMIT");
        } catch (...) {
            history.run("ROLLBACK");
            throw;
        }
        while (count--) pending.pop_front();
        refresh();
    }

    void createControls() {
        dpi = static_cast<int>(GetDpiForWindow(window));
        font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        status = control(L"STATIC", L"已连接", 0, SS_LEFT | SS_CENTERIMAGE);
        search = control(L"EDIT", L"", Search, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索历史记录"));
        group = control(L"COMBOBOX", L"", GroupList, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL);
        control(L"BUTTON", L"管理分组", GroupMenu, WS_TABSTOP);
        control(L"BUTTON", L"移动到…", Move, WS_TABSTOP);
        control(L"BUTTON", L"删除选中", Erase, WS_TABSTOP);
        control(L"BUTTON", L"清空历史", Clear, WS_TABSTOP);
        auto pin = control(L"BUTTON", L"置顶", Pin, BS_AUTOCHECKBOX | WS_TABSTOP);
        SendMessageW(pin, BM_SETCHECK, BST_CHECKED, 0);
        items = control(WC_LISTVIEWW, L"", Items, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(items, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(items, RGB(255,255,255));
        ListView_SetTextColor(items, Ink);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = px(143);
        col.pszText = const_cast<wchar_t*>(L"时间");
        ListView_InsertColumn(items, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"分组");
        ListView_InsertColumn(items, 1, &col);
        col.pszText = const_cast<wchar_t*>(L"内容");
        ListView_InsertColumn(items, 2, &col);
        preview = control(L"EDIT", L"", Preview, ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
        SendMessageW(preview, EM_SETLIMITTEXT, 0x7ffffffe, 0);
        control(L"BUTTON", L"复制选中内容", Copy, WS_TABSTOP);
        control(L"BUTTON", L"粘贴到光标", Paste, BS_OWNERDRAW | WS_TABSTOP);
        control(L"BUTTON", L"断开", Connect, WS_TABSTOP);
        control(L"BUTTON", L"收起 ▲", Fold, WS_TABSTOP);
        for (UINT id : {static_cast<UINT>(Items), static_cast<UINT>(Copy), static_cast<UINT>(Paste)})
            SetWindowSubclass(GetDlgItem(window, id), mouseControl, id, reinterpret_cast<DWORD_PTR>(this));
        fillGroups();
        refresh();
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        icon.uCallbackMessage = TrayMessage;
        icon.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_CLIPBOARD_HISTORY), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (!icon.hIcon) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        lstrcpynW(icon.szTip, L"粘贴板收纳盒 · Ctrl+Alt+V", ARRAYSIZE(icon.szTip));
        trayIcon();
        listener = AddClipboardFormatListener(window);
        lastSequence = GetClipboardSequenceNumber();
        hotkey = RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');
        statusText();
        SetTimer(window, 1, 100, nullptr);
        layout();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == taskbarCreated) { tray = false; trayIcon(); return 0; }
        switch (msg) {
        case WM_CREATE: createControls(); return 0;
        case WM_SIZE: if (status && wp != SIZE_MINIMIZED) layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {px(365), folded ? compactHeight() : px(380)};
            if (folded) info->ptMaxTrackSize.y = info->ptMinTrackSize.y;
            return 0;
        }
        case WM_ENTERSIZEMOVE: moving = true; return 0;
        case WM_EXITSIZEMOVE: moving = false; return 0;
        case WM_MOUSEACTIVATE: rememberTarget(); break;
        case WM_ERASEBKGND: {
            RECT area{}; GetClientRect(window, &area);
            FillRect(reinterpret_cast<HDC>(wp), &area, background);
            return 1;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
            auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, reinterpret_cast<HWND>(lp) == status && connected ? RGB(31,94,72) : Ink);
            SetBkColor(dc, msg == WM_CTLCOLOREDIT ? RGB(255,255,255) : Background);
            return reinterpret_cast<LRESULT>(msg == WM_CTLCOLOREDIT ? paper : background);
        }
        case WM_DRAWITEM: {
            auto draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (draw->CtlID != Paste) break;
            auto brush = CreateSolidBrush(draw->itemState & ODS_SELECTED ? RGB(0,77,166) : Accent);
            FillRect(draw->hDC, &draw->rcItem, brush);
            DeleteObject(brush);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, RGB(255,255,255));
            SelectObject(draw->hDC, font);
            DrawTextW(draw->hDC, L"粘贴到光标", -1, &draw->rcItem, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            if (draw->itemState & ODS_FOCUS) {
                RECT focus = draw->rcItem; InflateRect(&focus, -3, -3); DrawFocusRect(draw->hDC, &focus);
            }
            return TRUE;
        }
        case WM_NOTIFY: {
            auto header = reinterpret_cast<NMHDR*>(lp);
            if (header->idFrom == Items && header->code == LVN_ITEMCHANGED) {
                int row = selection();
                if (row >= 0 && static_cast<size_t>(row) < rows.size()) {
                    std::wstring display;
                    for (wchar_t ch : rows[row].text) {
                        if (ch == L'\n' && (display.empty() || display.back() != L'\r')) display += L'\r';
                        display += ch;
                    }
                    SetWindowTextW(preview, display.c_str());
                    SendMessageW(preview, EM_SETMODIFY, FALSE, 0);
                }
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case Search: if (HIWORD(wp) == EN_CHANGE && items) refresh(); break;
            case GroupList:
                if (HIWORD(wp) == CBN_SELCHANGE) {
                    refresh();
                    sizeHistoryColumns();
                }
                break;
            case GroupMenu: groupActions(GroupMenu); break;
            case Move: groupActions(Move); break;
            case Copy: copyOrPaste(false); break;
            case Paste: copyOrPaste(true); break;
            case Erase: {
                int index = selection();
                if (index < 0) statusText(L"请先选择要删除的记录");
                else { history.remove(rows[index].id); refresh(); }
                break;
            }
            case Clear:
                if (MessageBoxW(window, L"清空所有未分组记录？已分组的内容会保留。", L"清空历史",
                    MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES) {
                    history.clear(); refresh();
                }
                break;
            case Connect:
                connected = !connected;
                ++generation;
                lastSequence = GetClipboardSequenceNumber();
                SetWindowTextW(GetDlgItem(window, Connect), connected ? L"断开" : L"连接");
                statusText();
                break;
            case Pin:
                SetWindowPos(window, SendMessageW(GetDlgItem(window, Pin), BM_GETCHECK, 0, 0) == BST_CHECKED ? HWND_TOPMOST : HWND_NOTOPMOST,
                             0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
                break;
            case Fold: fold(); break;
            }
            return 0;
        case WM_CLIPBOARDUPDATE: capture(); return 0;
        case WM_TIMER:
            if (wp == 2) attemptPaste();
            else { rememberTarget(); flush(); }
            return 0;
        case WM_HOTKEY: show(); return 0;
        case TrayMessage:
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) show();
            else if (lp == WM_RBUTTONUP) {
                POINT point{}; GetCursorPos(&point);
                SetForegroundWindow(window);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, TrayOpen, L"打开");
                AppendMenuW(menu, MF_STRING, TrayExit, L"退出");
                auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, window, nullptr);
                DestroyMenu(menu);
                if (command == TrayOpen) show();
                else if (command == TrayExit) DestroyWindow(window);
                else PostMessageW(window, WM_NULL, 0, 0);
            }
            return 0;
        case WM_CLOSE: if (tray) ShowWindow(window, SW_HIDE); else DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            KillTimer(window, 2);
            if (listener) RemoveClipboardFormatListener(window);
            if (hotkey) UnregisterHotKey(window, 1);
            if (tray) Shell_NotifyIconW(NIM_DELETE, &icon);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, msg, wp, lp);
    }

    static LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, msg, wp, lp);
        try { return self->handle(msg, wp, lp); }
        catch (const std::exception&) {
            if (msg == WM_CREATE) return -1;
            if (self->status) self->statusText(L"历史记录暂时无法读取或写入");
            return 0;
        }
    }
public:
    explicit App(const fs::path& database) : history(database.c_str()) {}
    ~App() { DeleteObject(font); DeleteObject(background); DeleteObject(paper); }

    int run(HINSTANCE instance) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSW cls{};
        cls.lpfnWndProc = proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_CLIPBOARD_HISTORY));
        if (!cls.hIcon) cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        cls.lpszClassName = ClassName;
        if (!RegisterClassW(&cls)) throw std::runtime_error("window class");
        int scale = static_cast<int>(GetDpiForSystem());
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int w = std::min<int>(work.right-work.left, MulDiv(normalWidth, scale, 96));
        int h = std::min<int>(work.bottom-work.top, MulDiv(normalHeight, scale, 96));
        rememberTarget();
        HWND result = CreateWindowExW(WS_EX_TOPMOST, ClassName, AppName,
            WS_OVERLAPPEDWINDOW, work.right-w-MulDiv(18,scale,96), work.top+MulDiv(65,scale,96), w, h,
            nullptr, nullptr, instance, this);
        if (!result) throw std::runtime_error("create window");
        ShowWindow(window, SW_SHOWNOACTIVATE);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }
};

int selfTest() {
    History db(L":memory:", false);
    db.run("CREATE TABLE history(id INTEGER PRIMARY KEY, content TEXT NOT NULL, created TEXT DEFAULT 'old')");
    db.run("INSERT INTO history(content) VALUES ('旧记录')");
    db.ensureSchema();
    if (db.records(0, L"").at(0).time != L"old") return 10;
    db.add(L"项目");
    db.add(L"项目");
    if (db.records(-1, L"项目").size() != 2) return 11;
    db.nameGroup(0, L"常用");
    Id group = db.groups().at(0).id;
    db.move(1, group);
    db.clear();
    if (!db.records(0, L"").empty() || db.records(group, L"旧").size() != 1) return 12;
    db.nameGroup(group, L"项目资料");
    if (db.groups().at(0).name != L"项目资料") return 13;
    db.removeGroup(group);
    if (db.records(0, L"").size() != 1) return 14;
    db.remove(1);
    if (!db.records(-1, L"").empty()) return 15;

    db.nameGroup(0, L"a1");
    Id a1 = db.groups().at(0).id;
    db.add(L"活动分组内的复制", a1);
    db.add(L"全部记录视图的复制", -1);
    db.add(L"未分组视图的复制", 0);
    if (db.records(a1, L"活动分组").size() != 1 || db.records(0, L"").size() != 2)
        return 16;
    if (db.records(-1,L"活动分组").at(0).groupName != L"a1"
        || !db.records(-1,L"未分组视图").at(0).groupName.empty()) return 25;
    db.nameGroup(a1,L"项目一");
    if (db.records(-1,L"活动分组").at(0).groupName != L"项目一") return 26;
    db.clear();
    if (db.records(a1, L"").size() != 1 || !db.records(0, L"").empty()) return 17;
    db.removeGroup(a1);
    db.add(L"分组在保存前已删除", a1);
    if (db.records(0, L"").size() != 2) return 18;

    // Older databases can reject the default timestamp when localtime() is unavailable.
    History noDefault(L":memory:", false);
    noDefault.run("CREATE TABLE history(id INTEGER PRIMARY KEY, content TEXT NOT NULL, created TEXT NOT NULL DEFAULT NULL)");
    noDefault.ensureSchema();
    noDefault.add(L"写入时间");
    if (noDefault.records(-1, L"").at(0).time.size() != 19) return 19;

    History duplicates(L":memory:");
    duplicates.nameGroup(0, L"a1");
    duplicates.nameGroup(0, L"a2");
    auto groups = duplicates.groups();
    duplicates.add(L"编号A");
    duplicates.add(L"编号A ");
    duplicates.add(L"编号A", groups[0].id);
    duplicates.add(L"编号A");
    duplicates.add(L"编号A", groups[1].id);
    if (duplicates.records(0,L"").size() != 1 || duplicates.records(0,L"").at(0).text != L"编号A "
        || duplicates.records(groups[0].id,L"").size() != 1 || duplicates.records(groups[1].id,L"").size() != 1) return 21;
    duplicates.add(L"移动");
    duplicates.add(L"移动");
    duplicates.move(duplicates.records(0,L"移动").at(0).id, groups[0].id);
    if (!duplicates.records(0,L"移动").empty() || duplicates.records(groups[0].id,L"移动").size() != 1) return 22;
    duplicates.run("INSERT INTO history(content,created) VALUES ('编号A','existing')");
    duplicates.ensureSchema();
    if (duplicates.records(0,L"").size() != 1) return 23;
    duplicates.removeGroup(groups[0].id);
    return duplicates.records(0,L"编号A").size() == 1 && duplicates.records(0,L"移动").size() == 1 ? 0 : 24;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    SetProcessDPIAware();
    try {
        if (std::wstring(arguments) == L"--self-test") return selfTest();
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 1;
        auto directory = fs::path(executable).parent_path();
        auto data = directory.filename() == L"app" ? directory.parent_path() : directory;
        auto key = std::hash<std::wstring>{}(data.wstring());
        auto mutexName = L"Local\\ClipboardCpp_" + std::to_wstring(key);
        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (!mutex) return 2;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex);
            if (HWND existing = FindWindowW(ClassName, AppName)) PostMessageW(existing, WM_HOTKEY, 1, 0);
            return 0;
        }
        App app(data / L"剪贴板历史.db");
        int exitCode = app.run(instance);
        CloseHandle(mutex);
        return exitCode;
    } catch (...) {
        if (std::wstring(arguments) == L"--self-test") return 20;
        MessageBoxW(nullptr, L"无法启动粘贴板收纳盒。请检查程序目录的写入权限，并退出仍在使用历史数据库的旧版。", AppName, MB_OK | MB_ICONERROR);
        return 1;
    }
}
