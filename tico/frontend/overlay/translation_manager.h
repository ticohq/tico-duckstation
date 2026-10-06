// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace SwitchFrontend::OverlayTranslation {

class TranslationManager {
public:
    static TranslationManager& Instance();

    bool Init();
    bool IsLoaded() const { return !m_translations.empty(); }
    std::string GetString(const std::string& key) const;
    // Every loaded string, e.g. to know which characters the fonts need.
    void ForEachString(const std::function<void(const std::string&)>& fn) const;
    // The language file tico is set to (en.json, ja.json, ...).
    std::string LanguageFile() const;

private:
    TranslationManager() = default;

    // Adds the file's strings, replacing ones already loaded.
    bool LoadLanguageFile(const std::string& filename);

    std::string m_current_language;
    std::unordered_map<std::string, std::string> m_translations;
};

std::string tr(const std::string& key);

} // namespace SwitchFrontend::OverlayTranslation
