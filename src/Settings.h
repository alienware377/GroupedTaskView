#pragma once

#include <string>
#include <vector>

// A user-defined grouping rule. Any window whose app identity (executable path /
// AppUserModelID) or app name contains `match` (case-insensitive) is placed in a
// group named `name`. One rule with a unique match renames a single app's group;
// several rules sharing the same `name` merge those apps into one group.
struct GroupRule
{
    std::wstring match;
    std::wstring name;
};

namespace Settings
{
    std::wstring FilePath(); // %LOCALAPPDATA%\GroupedTaskView\settings.ini

    std::vector<GroupRule> LoadRules();
    void SaveRules(const std::vector<GroupRule>& rules);

    // If a rule matches, fills outKey (a merge key) and outName and returns true.
    bool ApplyRules(const std::vector<GroupRule>& rules,
                    const std::wstring& identity,
                    const std::wstring& appName,
                    std::wstring& outKey,
                    std::wstring& outName);
}
