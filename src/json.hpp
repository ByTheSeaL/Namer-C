// Minimal JSON parser/serializer (UTF-8, stdlib only).
//
// Enough JSON for talking to OpenRouter: objects, arrays, strings (with
// \uXXXX escapes incl. surrogate pairs), numbers, bools, null. Parse errors
// throw std::runtime_error.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    // Object member lookup; nullptr if not an object or key absent.
    const Json* get(const std::string& key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    std::string asString() const { return type == String ? str : std::string(); }

    static Json parse(const std::string& text) {
        Parser p{text, 0};
        Json v = p.value();
        p.skipWs();
        if (p.pos != text.size())
            throw std::runtime_error("JSON: trailing characters");
        return v;
    }

    // Serialize (only what we need for request bodies).
    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

    static Json makeString(const std::string& s) {
        Json j; j.type = String; j.str = s; return j;
    }
    static Json makeNumber(double n) {
        Json j; j.type = Number; j.number = n; return j;
    }
    void set(const std::string& key, Json v) {
        type = Object;
        obj.emplace_back(key, std::move(v));
    }

private:
    struct Parser {
        const std::string& s;
        size_t pos;

        void skipWs() {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                                      s[pos] == '\n' || s[pos] == '\r'))
                pos++;
        }
        char peek() {
            if (pos >= s.size()) throw std::runtime_error("JSON: unexpected end");
            return s[pos];
        }
        void expect(char c) {
            if (pos >= s.size() || s[pos] != c)
                throw std::runtime_error(std::string("JSON: expected '") + c + "'");
            pos++;
        }
        bool consume(const char* lit) {
            size_t n = strlen(lit);
            if (s.compare(pos, n, lit) == 0) { pos += n; return true; }
            return false;
        }

        Json value() {
            skipWs();
            char c = peek();
            Json v;
            if (c == '{') {
                pos++; v.type = Json::Object;
                skipWs();
                if (peek() == '}') { pos++; return v; }
                for (;;) {
                    skipWs();
                    std::string key = string();
                    skipWs();
                    expect(':');
                    v.obj.emplace_back(std::move(key), value());
                    skipWs();
                    if (peek() == ',') { pos++; continue; }
                    expect('}');
                    return v;
                }
            }
            if (c == '[') {
                pos++; v.type = Json::Array;
                skipWs();
                if (peek() == ']') { pos++; return v; }
                for (;;) {
                    v.arr.push_back(value());
                    skipWs();
                    if (peek() == ',') { pos++; continue; }
                    expect(']');
                    return v;
                }
            }
            if (c == '"') { v.type = Json::String; v.str = string(); return v; }
            if (consume("true"))  { v.type = Json::Bool; v.boolean = true;  return v; }
            if (consume("false")) { v.type = Json::Bool; v.boolean = false; return v; }
            if (consume("null"))  { return v; }
            // number
            size_t start = pos;
            if (peek() == '-') pos++;
            while (pos < s.size() && (isdigit((unsigned char)s[pos]) || s[pos] == '.' ||
                                      s[pos] == 'e' || s[pos] == 'E' ||
                                      s[pos] == '+' || s[pos] == '-'))
                pos++;
            if (pos == start) throw std::runtime_error("JSON: bad value");
            v.type = Json::Number;
            v.number = strtod(s.substr(start, pos - start).c_str(), nullptr);
            return v;
        }

        std::string string() {
            expect('"');
            std::string out;
            while (true) {
                if (pos >= s.size()) throw std::runtime_error("JSON: unterminated string");
                char c = s[pos++];
                if (c == '"') return out;
                if (c != '\\') { out += c; continue; }
                if (pos >= s.size()) throw std::runtime_error("JSON: bad escape");
                char e = s[pos++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        uint32_t cp = hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                            if (pos + 1 < s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                                pos += 2;
                                uint32_t lo = hex4();
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: throw std::runtime_error("JSON: bad escape");
                }
            }
        }

        uint32_t hex4() {
            if (pos + 4 > s.size()) throw std::runtime_error("JSON: bad \\u escape");
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                char c = s[pos++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else throw std::runtime_error("JSON: bad \\u escape");
            }
            return v;
        }

        static void appendUtf8(std::string& out, uint32_t cp) {
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }
    };

    void write(std::string& out) const {
        switch (type) {
            case Null:   out += "null"; break;
            case Bool:   out += boolean ? "true" : "false"; break;
            case Number: {
                char buf[32];
                if (number == (long long)number)
                    snprintf(buf, sizeof buf, "%lld", (long long)number);
                else
                    snprintf(buf, sizeof buf, "%g", number);
                out += buf;
                break;
            }
            case String: writeString(out, str); break;
            case Array:
                out += '[';
                for (size_t i = 0; i < arr.size(); i++) {
                    if (i) out += ',';
                    arr[i].write(out);
                }
                out += ']';
                break;
            case Object:
                out += '{';
                for (size_t i = 0; i < obj.size(); i++) {
                    if (i) out += ',';
                    writeString(out, obj[i].first);
                    out += ':';
                    obj[i].second.write(out);
                }
                out += '}';
                break;
        }
    }

    static void writeString(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out += (char)c;
                    }
            }
        }
        out += '"';
    }
};
