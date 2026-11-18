#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;
using Id = std::int64_t;
struct Group { Id id; std::wstring name; };
struct Record { Id id; std::wstring time, text, groupName; };

class History {
    template<class T> static T load(HMODULE library, const char* name) {
        auto address = GetProcAddress(library, name);
        if (!address) throw std::runtime_error(name);
        return reinterpret_cast<T>(address);
    }

    HMODULE library = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    sqlite3* db = nullptr;
    int (*open)(const void*, sqlite3**) = load<decltype(open)>(library, "sqlite3_open16");
    int (*close)(sqlite3*) = load<decltype(close)>(library, "sqlite3_close");
    int (*exec)(sqlite3*, const char*, void*, void*, char**) = load<decltype(exec)>(library, "sqlite3_exec");
    int (*prepare)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = load<decltype(prepare)>(library, "sqlite3_prepare_v2");
    int (*finalize)(sqlite3_stmt*) = load<decltype(finalize)>(library, "sqlite3_finalize");
    int (*step)(sqlite3_stmt*) = load<decltype(step)>(library, "sqlite3_step");
    int (*bindId)(sqlite3_stmt*, int, Id) = load<decltype(bindId)>(library, "sqlite3_bind_int64");
    int (*bindText)(sqlite3_stmt*, int, const void*, int, void(*)(void*)) = load<decltype(bindText)>(library, "sqlite3_bind_text16");
    Id (*readId)(sqlite3_stmt*, int) = load<decltype(readId)>(library, "sqlite3_column_int64");
    const void* (*readText)(sqlite3_stmt*, int) = load<decltype(readText)>(library, "sqlite3_column_text16");
    const char* (*error)(sqlite3*) = load<decltype(error)>(library, "sqlite3_errmsg");
    int (*timeout)(sqlite3*, int) = load<decltype(timeout)>(library, "sqlite3_busy_timeout");

    void check(int status) { if (status != 0) throw std::runtime_error(db ? error(db) : "SQLite unavailable"); }
    struct Query {
        History& owner;
        sqlite3_stmt* statement = nullptr;
        Query(History& h, const char* sql) : owner(h) { owner.check(owner.prepare(owner.db, sql, -1, &statement, nullptr)); }
        ~Query() { owner.finalize(statement); }
        void id(int pos, Id value) { owner.check(owner.bindId(statement, pos, value)); }
        void text(int pos, const std::wstring& value) {
            owner.check(owner.bindText(statement, pos, value.c_str(), -1, reinterpret_cast<void(*)(void*)>(-1)));
        }
        bool next() {
            int status = owner.step(statement);
            if (status == 100) return true;
            if (status != 101) owner.check(status);
            return false;
        }
        Id number(int col) const { return owner.readId(statement, col); }
        std::wstring value(int col) const {
            auto text = static_cast<const wchar_t*>(owner.readText(statement, col));
            return text ? text : L"";
        }
    };

public:
    explicit History(const wchar_t* path, bool migrate = true) {
        try {
            check(open(path, &db));
            timeout(db, 80);
            if (migrate) ensureSchema();
        } catch (...) {
            if (db) close(db);
            FreeLibrary(library);
            throw;
        }
    }
    History(const History&) = delete;
    ~History() { close(db); FreeLibrary(library); }

    void ensureSchema() {
        run("BEGIN IMMEDIATE");
        try {
            run("CREATE TABLE IF NOT EXISTS history(id INTEGER PRIMARY KEY, content TEXT NOT NULL, created TEXT NOT NULL DEFAULT (datetime('now','localtime')), group_id INTEGER)");
            run("CREATE TABLE IF NOT EXISTS groups(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)");
            bool hasGroup = false;
            {
                Query columns(*this, "PRAGMA table_info(history)");
                while (columns.next()) hasGroup |= columns.value(1) == L"group_id";
            }
            if (!hasGroup) run("ALTER TABLE history ADD COLUMN group_id INTEGER");
            removeUngroupedCopies();
            run("COMMIT");
        } catch (...) {
            run("ROLLBACK");
            throw;
        }
    }
    void run(const char* sql) { check(exec(db, sql, nullptr, nullptr, nullptr)); }
    std::vector<Group> groups() {
        Query q(*this, "SELECT id,name FROM groups ORDER BY name COLLATE NOCASE");
        std::vector<Group> result;
        while(q.next()) result.push_back({q.number(0), q.value(1)});
        return result;
    }
    std::vector<Record> records(Id group, const std::wstring& search) {
        Query q(*this, "SELECT h.id,h.created,h.content,g.name FROM history h "
                       "LEFT JOIN groups g ON g.id=h.group_id WHERE instr(h.content,?1)>0 "
                       "AND (?2=-1 OR (?2=0 AND h.group_id IS NULL) OR h.group_id=?2) "
                       "ORDER BY h.id DESC LIMIT 1000");
        q.text(1, search);
        q.id(2, group);
        std::vector<Record> result;
        while(q.next()) result.push_back({q.number(0), q.value(1), q.value(2), q.value(3)});
        return result;
    }
    void add(const std::wstring& text, Id group = 0) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t created[32]{};
        std::swprintf(created, sizeof(created) / sizeof(*created), L"%04u-%02u-%02u %02u:%02u:%02u",
            static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
            static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
            static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond));
        Query q(*this, "INSERT INTO history(content,created,group_id) "
                       "SELECT ?1,?2,(SELECT id FROM groups WHERE id=NULLIF(?3,0)) "
                       "WHERE EXISTS (SELECT 1 FROM groups WHERE id=NULLIF(?3,0)) "
                       "OR NOT EXISTS (SELECT 1 FROM history h JOIN groups g ON g.id=h.group_id WHERE h.content=?1 COLLATE BINARY)");
        q.text(1, text);
        q.text(2, created);
        q.id(3, group > 0 ? group : 0);
        q.next();
        if (group > 0) {
            Query duplicates(*this, "DELETE FROM history WHERE group_id IS NULL AND content=?1 COLLATE BINARY "
                                    "AND EXISTS (SELECT 1 FROM history h JOIN groups g ON g.id=h.group_id WHERE h.content=?1 COLLATE BINARY)");
            duplicates.text(1, text);
            duplicates.next();
        }
    }
    void nameGroup(Id id, const std::wstring& name) {
        Query q(*this, id ? "UPDATE groups SET name=?1 WHERE id=?2" : "INSERT INTO groups(name) VALUES (?1)");
        q.text(1, name);
        if (id) q.id(2, id);
        q.next();
    }
    void move(Id item, Id group) {
        run("BEGIN IMMEDIATE");
        try {
            Query q(*this, "UPDATE history SET group_id=(SELECT id FROM groups WHERE id=NULLIF(?1,0)) WHERE id=?2");
            q.id(1, group);
            q.id(2, item);
            q.next();
            Query duplicates(*this,
                "DELETE FROM history WHERE group_id IS NULL AND content COLLATE BINARY=(SELECT content FROM history WHERE id=?1) "
                "AND EXISTS (SELECT 1 FROM history h JOIN groups g ON g.id=h.group_id WHERE h.content=history.content COLLATE BINARY)");
            duplicates.id(1, item);
            duplicates.next();
            run("COMMIT");
        } catch (...) { run("ROLLBACK"); throw; }
    }
    void removeGroup(Id group) {
        run("BEGIN IMMEDIATE");
        try {
            Query update(*this, "UPDATE history SET group_id=NULL WHERE group_id=?");
            update.id(1, group); update.next();
            Query remove(*this, "DELETE FROM groups WHERE id=?");
            remove.id(1, group); remove.next();
            removeUngroupedCopies();
            run("COMMIT");
        } catch (...) { run("ROLLBACK"); throw; }
    }
    void remove(Id item) {
        Query q(*this, "DELETE FROM history WHERE id=?"); q.id(1, item); q.next();
    }
    void clear() { run("DELETE FROM history WHERE group_id IS NULL"); }

private:
    void removeUngroupedCopies() {
        run("DELETE FROM history WHERE group_id IS NULL AND content COLLATE BINARY IN "
            "(SELECT h.content FROM history h JOIN groups g ON g.id=h.group_id)");
    }
};
