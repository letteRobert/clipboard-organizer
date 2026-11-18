#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

class ClipboardMonitor {
public:
    struct Entry { unsigned generation; std::int64_t group; std::wstring text; };
private:
    struct State {
        std::mutex mutex;
        std::condition_variable signal;
        bool stop = false;
        bool ready = false;
        DWORD sequence = 0;
        unsigned generation = 0;
        std::int64_t group = 0;
        std::deque<Entry> entries;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    static std::wstring read(DWORD sequence) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (sequence != GetClipboardSequenceNumber()) return {};
            if (OpenClipboard(nullptr)) {
                std::wstring result;
                DWORD owner = 0;
                GetWindowThreadProcessId(GetClipboardOwner(), &owner);
                if (sequence == GetClipboardSequenceNumber() && owner != GetCurrentProcessId()
                    && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                    if (auto data = GetClipboardData(CF_UNICODETEXT)) {
                        if (auto text = static_cast<const wchar_t*>(GlobalLock(data))) {
                            auto count = GlobalSize(data) / sizeof(wchar_t);
                            auto end = std::wmemchr(text, L'\0', count);
                            if (end) result.assign(text, end);
                            GlobalUnlock(data);
                        }
                    }
                }
                CloseClipboard();
                return result;
            }
            Sleep(8);
        }
        return {};
    }
public:
    ClipboardMonitor() {
        std::thread([s = state] {
            std::unique_lock lock(s->mutex);
            while (true) {
                s->signal.wait(lock, [&] { return s->ready || s->stop; });
                if (s->stop) return;
                auto seq = s->sequence;
                auto generation = s->generation;
                auto group = s->group;
                s->ready = false;
                lock.unlock();
                auto text = read(seq);
                lock.lock();
                if (!text.empty() && s->entries.size() < 1024)
                    s->entries.push_back({generation, group, std::move(text)});
            }
        }).detach();
    }
    ~ClipboardMonitor() {
        std::lock_guard lock(state->mutex);
        state->stop = true;
        state->signal.notify_one();
    }
    void notify(DWORD seq, unsigned generation, std::int64_t group) {
        std::lock_guard lock(state->mutex);
        state->sequence = seq;
        state->generation = generation;
        state->group = group;
        state->ready = true;
        state->signal.notify_one();
    }
    std::deque<Entry> drain() {
        std::lock_guard lock(state->mutex);
        std::deque<Entry> result;
        result.swap(state->entries);
        return result;
    }

    static bool write(HWND owner, const std::wstring& text, DWORD& sequence) {
        const auto bytes = (text.size() + 1) * sizeof(wchar_t);
        auto data = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!data) return false;
        auto buffer = GlobalLock(data);
        if (!buffer) { GlobalFree(data); return false; }
        std::memcpy(buffer, text.c_str(), bytes);
        GlobalUnlock(data);
        if (!OpenClipboard(owner)) { GlobalFree(data); return false; }
        bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data);
        CloseClipboard();
        if (ok) sequence = GetClipboardSequenceNumber();
        if (!ok) GlobalFree(data);
        return ok;
    }
};
