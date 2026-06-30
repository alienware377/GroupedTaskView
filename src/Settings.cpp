#include "pch.h"
#include "Settings.h"

#include <shlobj.h>
#include <cwctype>

#pragma comment(lib, "shell32.lib")

namespace
{
    std::wstring ToLower(std::wstring s)
    {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    }

    std::wstring Dir()
    {
        PWSTR p = nullptr;
        std::wstring d;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p)
        {
            d = p;
            d += L"\\GroupedTaskView";
        }
        if (p) CoTaskMemFree(p);
        return d;
    }
}

namespace Settings
{
    std::wstring FilePath()
    {
        const std::wstring dir = Dir();
        if (!dir.empty())
        {
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            return dir + L"\\settings.ini";
        }
        return L"";
    }

    std::vector<GroupRule> LoadRules()
    {
        std::vector<GroupRule> rules;
        const std::wstring path = FilePath();
        if (path.empty()) return rules;

        const int count = GetPrivateProfileIntW(L"Rules", L"count", 0, path.c_str());
        for (int i = 0; i < count && i < 1000; ++i)
        {
            wchar_t match[512] = {};
            wchar_t name[512] = {};
            const std::wstring km = L"match" + std::to_wstring(i);
            const std::wstring kn = L"name" + std::to_wstring(i);
            GetPrivateProfileStringW(L"Rules", km.c_str(), L"", match, ARRAYSIZE(match), path.c_str());
            GetPrivateProfileStringW(L"Rules", kn.c_str(), L"", name, ARRAYSIZE(name), path.c_str());
            if (match[0] && name[0])
            {
                rules.push_back(GroupRule{ match, name });
            }
        }
        return rules;
    }

    void SaveRules(const std::vector<GroupRule>& rules)
    {
        const std::wstring path = FilePath();
        if (path.empty()) return;

        // Clear the whole section, then rewrite.
        WritePrivateProfileStringW(L"Rules", nullptr, nullptr, path.c_str());
        WritePrivateProfileStringW(L"Rules", L"count", std::to_wstring(rules.size()).c_str(), path.c_str());
        for (size_t i = 0; i < rules.size(); ++i)
        {
            const std::wstring km = L"match" + std::to_wstring(i);
            const std::wstring kn = L"name" + std::to_wstring(i);
            WritePrivateProfileStringW(L"Rules", km.c_str(), rules[i].match.c_str(), path.c_str());
            WritePrivateProfileStringW(L"Rules", kn.c_str(), rules[i].name.c_str(), path.c_str());
        }
    }

    bool OverrideSystemTaskView()
    {
        const std::wstring path = FilePath();
        if (path.empty()) return false;
        return GetPrivateProfileIntW(L"General", L"OverrideSystemTaskView", 0, path.c_str()) != 0;
    }

    void SetOverrideSystemTaskView(bool enabled)
    {
        const std::wstring path = FilePath();
        if (path.empty()) return;
        WritePrivateProfileStringW(L"General", L"OverrideSystemTaskView", enabled ? L"1" : L"0", path.c_str());
    }

    bool ApplyRules(const std::vector<GroupRule>& rules,
                    const std::wstring& identity,
                    const std::wstring& appName,
                    std::wstring& outKey,
                    std::wstring& outName)
    {
        const std::wstring id = ToLower(identity);
        const std::wstring nm = ToLower(appName);
        for (const auto& rule : rules)
        {
            if (rule.match.empty()) continue;
            const std::wstring m = ToLower(rule.match);
            if (id.find(m) != std::wstring::npos || nm.find(m) != std::wstring::npos)
            {
                outName = rule.name;
                outKey = L"rule:" + ToLower(rule.name);
                return true;
            }
        }
        return false;
    }
}
