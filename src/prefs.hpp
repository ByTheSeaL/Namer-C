// Tiny JSON preferences store: last model + recently chosen models.
// Same file and keys as the Python Namer (%APPDATA%\Namer\prefs.json).
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "json.hpp"
#include "util.hpp"

namespace prefs {

constexpr size_t MAX_RECENT = 5;

struct Prefs {
    std::string lastModel;
    std::vector<std::string> recentModels;
};

inline std::wstring prefsFile() { return configDir() + L"\\prefs.json"; }

inline Prefs load() {
    Prefs p;
    try {
        Json j = Json::parse(readFileUtf8(prefsFile()));
        if (const Json* m = j.get("last_model"); m && m->type == Json::String)
            p.lastModel = m->str;
        if (const Json* r = j.get("recent_models"); r && r->type == Json::Array)
            for (const Json& v : r->arr)
                if (v.type == Json::String) p.recentModels.push_back(v.str);
    } catch (...) {
    }
    return p;
}

inline void save(const Prefs& p) {
    Json recents; recents.type = Json::Array;
    for (const std::string& m : p.recentModels)
        recents.arr.push_back(Json::makeString(m));
    Json j;
    j.set("last_model", Json::makeString(p.lastModel));
    j.set("recent_models", recents);
    writeFileUtf8(prefsFile(), j.dump());
}

// Move-to-front, capped at MAX_RECENT; persists.
inline void rememberModel(Prefs& p, const std::string& model) {
    p.lastModel = model;
    auto& r = p.recentModels;
    r.erase(std::remove(r.begin(), r.end(), model), r.end());
    r.insert(r.begin(), model);
    if (r.size() > MAX_RECENT) r.resize(MAX_RECENT);
    save(p);
}

}  // namespace prefs
