// LLM-backed name generation via OpenRouter, over WinHTTP.
//
// Auth: OPENROUTER_API_KEY env var, or the first line of
// %APPDATA%\Namer\openrouter_key (shared with the Python Namer).
// All functions here are blocking — call them from a worker thread.
#pragma once

#include <winhttp.h>

#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"
#include "util.hpp"

namespace llm {

constexpr wchar_t API_HOST[] = L"openrouter.ai";
constexpr wchar_t API_PATH[] = L"/api/v1";
constexpr DWORD TIMEOUT_MS = 90000;

inline const char* FALLBACK_MODELS[] = {
    "anthropic/claude-sonnet-4.5",
    "anthropic/claude-haiku-4.5",
    "openai/gpt-4o-mini",
    "google/gemini-2.5-flash",
    "meta-llama/llama-3.3-70b-instruct",
};

constexpr char SYSTEM[] =
    "You are an expert namer. You generate name ideas for anything - code "
    "identifiers, fictional characters and places, technical paper titles, products, "
    "projects. Given a description and a context, produce distinctive, memorable, "
    "apt names. Avoid generic or overused patterns. Vary your approaches: metaphor, "
    "portmanteau, classical roots, sound symbolism, allusion.\n\n"
    "Respond with ONLY a JSON object of the form:\n"
    "{\"names\": [{\"name\": \"...\", \"rationale\": \"...\"}, ...]}";

struct ContextGuidance { const char* context; const char* guidance; };
inline const ContextGuidance CONTEXT_GUIDANCE[] = {
    {"Code", "Names are code identifiers. Follow programming conventions: offer a mix "
             "of camelCase, snake_case, and PascalCase as appropriate. Favor short, "
             "precise, unambiguous names a reviewer would approve of."},
    {"Fiction", "Names for creative fiction: characters, places, factions, artifacts. "
                "Favor evocative sound and connotation over literal meaning. Consider "
                "etymology and how the name feels spoken aloud."},
    {"Paper / Technical", "Names for papers, systems, algorithms, or datasets. Favor "
                          "pronounceable acronyms, classical roots, and names that hint "
                          "at the method. Think BERT, RAFT, Paxos."},
    {"Product / Project", "Product or project names. Favor short, brandable, spellable "
                          "names. Flag any that are likely trademark-crowded."},
    {"General", "General-purpose naming. Offer a diverse spread of styles."},
};

// Instruction template for "Show similar" on a result the user liked.
// {name} is substituted with the chosen name.
constexpr char ITERATE_SIMILAR[] =
    "The name \"{name}\" is close to what the user wants. Generate "
    "more names like it: close variations and permutations, and "
    "names that build on the same idea, root, metaphor, feel, "
    "and style.";

struct NameIdea {
    std::string name;
    std::string rationale;
};

inline std::wstring keyFile() { return configDir() + L"\\openrouter_key"; }

inline std::string getApiKey() {
    wchar_t env[512];
    DWORD n = GetEnvironmentVariableW(L"OPENROUTER_API_KEY", env, 512);
    if (n > 0 && n < 512) return narrow(env);
    std::string content = readFileUtf8(keyFile());
    size_t end = content.find_first_of("\r\n");
    std::string key = content.substr(0, end);
    // trim
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    size_t start = key.find_first_not_of(" \t");
    return start == std::string::npos ? "" : key.substr(start);
}

inline void saveApiKey(const std::string& key) {
    writeFileUtf8(keyFile(), key.empty() ? "" : key + "\n");
}

// Blocking HTTPS request to the OpenRouter API. GET when body is empty.
// Returns the response body; throws std::runtime_error on transport errors.
inline std::string request(const std::wstring& path, const std::string& body) {
    HINTERNET session = WinHttpOpen(L"Namer-C",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");
    WinHttpSetTimeouts(session, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS);

    std::string result;
    std::string error;
    HINTERNET conn = nullptr, req = nullptr;
    do {
        conn = WinHttpConnect(session, API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!conn) { error = "Could not connect to openrouter.ai"; break; }

        std::wstring fullPath = std::wstring(API_PATH) + path;
        req = WinHttpOpenRequest(conn, body.empty() ? L"GET" : L"POST",
                                 fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) { error = "Could not create request"; break; }

        std::wstring headers = L"Content-Type: application/json\r\n"
                               L"HTTP-Referer: https://github.com/namer-app\r\n"
                               L"X-Title: Namer\r\n";
        std::string key = getApiKey();
        if (!key.empty())
            headers += L"Authorization: Bearer " + widen(key) + L"\r\n";

        if (!WinHttpSendRequest(req, headers.c_str(), (DWORD)-1,
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            DWORD err = GetLastError();
            error = err == ERROR_WINHTTP_TIMEOUT
                        ? "Request timed out"
                        : "Network error (WinHTTP " + std::to_string(err) + ")";
            break;
        }

        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, 0);
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got)) break;
            result.append(chunk.data(), got);
        }
    } while (false);

    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (!error.empty()) throw std::runtime_error(error);
    return result;
}

// Fetch available model ids (no auth required); fallback list on any failure.
inline std::vector<std::string> listModels() {
    std::vector<std::string> ids;
    try {
        Json data = Json::parse(request(L"/models", ""));
        const Json* list = data.get("data");
        if (list && list->type == Json::Array) {
            for (const Json& m : list->arr) {
                const Json* id = m.get("id");
                if (id && id->type == Json::String) ids.push_back(id->str);
            }
        }
        std::sort(ids.begin(), ids.end());
    } catch (...) {
    }
    if (ids.empty())
        for (const char* m : FALLBACK_MODELS) ids.push_back(m);
    return ids;
}

// Ask the chosen model for (name, rationale) pairs.
// extra: optional follow-up instruction (e.g. ITERATE_SIMILAR with a name
// substituted in). Throws std::runtime_error with a user-facing message.
inline std::vector<NameIdea> suggest(const std::string& description,
                                     const std::string& context,
                                     const std::string& model,
                                     const std::string& extra = "",
                                     int count = 12) {
    const char* guidance = "";
    for (const auto& g : CONTEXT_GUIDANCE)
        if (context == g.context) { guidance = g.guidance; break; }

    std::string prompt = "Context: " + context + ". " + guidance +
                         "\n\nThing to name: " + description + "\n\n" +
                         (extra.empty() ? "" : extra + "\n\n") +
                         "Give " + std::to_string(count) + " name ideas.";

    Json sys; sys.set("role", Json::makeString("system"));
    sys.set("content", Json::makeString(SYSTEM));
    Json usr; usr.set("role", Json::makeString("user"));
    usr.set("content", Json::makeString(prompt));
    Json msgs; msgs.type = Json::Array;
    msgs.arr.push_back(sys);
    msgs.arr.push_back(usr);

    Json payload;
    payload.set("model", Json::makeString(model));
    payload.set("max_tokens", Json::makeNumber(2048));
    payload.set("messages", msgs);

    Json data = Json::parse(request(L"/chat/completions", payload.dump()));

    // OpenRouter can return an error object inside a 200 response.
    if (const Json* err = data.get("error")) {
        const Json* msg = err->get("message");
        throw std::runtime_error(msg && msg->type == Json::String
                                     ? msg->str : err->dump());
    }
    const Json* choices = data.get("choices");
    if (!choices || choices->type != Json::Array || choices->arr.empty())
        throw std::runtime_error("Model returned no choices - try a different model.");

    // content can be null (e.g. reasoning-only replies from some free models)
    const Json* message = choices->arr[0].get("message");
    std::string text;
    if (message) {
        if (const Json* c = message->get("content"); c && c->type == Json::String)
            text = c->str;
        if (text.empty())
            if (const Json* r = message->get("reasoning"); r && r->type == Json::String)
                text = r->str;
    }

    // Models sometimes wrap JSON in markdown fences or prose — extract the object.
    size_t open = text.find('{');
    size_t close = text.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open)
        throw std::runtime_error(
            text.empty() ? "Model returned an empty response - try a different model."
                         : "Model returned no JSON - try a different model.");

    Json parsed = Json::parse(text.substr(open, close - open + 1));
    const Json* names = parsed.get("names");
    if (!names || names->type != Json::Array)
        throw std::runtime_error("Model reply was missing \"names\" - try again.");

    std::vector<NameIdea> out;
    for (const Json& item : names->arr) {
        const Json* n = item.get("name");
        if (!n || n->type != Json::String) continue;
        const Json* r = item.get("rationale");
        out.push_back({n->str, r && r->type == Json::String ? r->str : ""});
    }
    if (out.empty()) throw std::runtime_error("Model returned no names - try again.");
    return out;
}

}  // namespace llm
