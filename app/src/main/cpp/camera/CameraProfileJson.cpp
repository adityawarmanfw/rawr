#include "camera/CameraProfileJson.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rawrcam::camera {
namespace {

// Minimal JSON reader: enough for profile documents written by the app.
struct Json {
    enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;        // Array elements, or object values.
    std::vector<std::string> keys;  // Object keys, parallel to items.

    const Json* get(const char* key) const {
        if (kind != Kind::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
    std::string str(const char* key, std::string fallback = {}) const {
        const Json* v = get(key);
        return v && v->kind == Kind::String ? v->text : fallback;
    }
    double num(const char* key, double fallback = 0.0) const {
        const Json* v = get(key);
        return v && v->kind == Kind::Number ? v->number : fallback;
    }
    bool flag(const char* key, bool fallback) const {
        const Json* v = get(key);
        return v && v->kind == Kind::Bool ? v->boolean : fallback;
    }
    const std::vector<Json>& array(const char* key) const {
        static const std::vector<Json> empty;
        const Json* v = get(key);
        return v && v->kind == Kind::Array ? v->items : empty;
    }
};

class Parser {
   public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        space();
        return i_ == s_.size() || fail("trailing characters");
    }
    std::string error;

   private:
    bool fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at " + std::to_string(i_);
        return false;
    }
    void space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool literal(const char* word) {
        const std::string w(word);
        if (s_.compare(i_, w.size(), w) != 0) return fail("unexpected token");
        i_ += w.size();
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > 32) return fail("nesting too deep");
        space();
        if (i_ >= s_.size()) return fail("unexpected end");
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (c == 't' || c == 'f') {
            out.kind = Json::Kind::Bool;
            out.boolean = c == 't';
            return literal(c == 't' ? "true" : "false");
        }
        if (c == 'n') {
            out.kind = Json::Kind::Null;
            return literal("null");
        }
        return numberValue(out);
    }
    bool numberValue(Json& out) {
        const char* begin = s_.c_str() + i_;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin || !std::isfinite(v)) return fail("invalid number");
        i_ += static_cast<size_t>(end - begin);
        out.kind = Json::Kind::Number;
        out.number = v;
        return true;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& cp) {
        if (i_ + 4 > s_.size()) return fail("short unicode escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9')
                cp |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f')
                cp |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                cp |= uint32_t(h - 'A' + 10);
            else
                return fail("invalid unicode escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++i_;  // opening quote
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                        i_ += 2;
                        uint32_t low = 0;
                        if (!hex4(low)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }
    bool array(Json& out, int depth) {
        out.kind = Json::Kind::Array;
        ++i_;
        space();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            space();
            if (i_ >= s_.size()) return fail("unterminated array");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("expected , or ]");
        }
    }
    bool object(Json& out, int depth) {
        out.kind = Json::Kind::Object;
        ++i_;
        space();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            space();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected key");
            out.keys.emplace_back();
            if (!string(out.keys.back())) return false;
            space();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected :");
            ++i_;
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            space();
            if (i_ >= s_.size()) return fail("unterminated object");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("expected , or }");
        }
    }
    const std::string& s_;
    size_t i_ = 0;
};

const char* typeName(CameraKeySetting::Type t) {
    switch (t) {
        case CameraKeySetting::Type::Byte:
            return "byte";
        case CameraKeySetting::Type::Int32:
            return "int32";
        case CameraKeySetting::Type::Int64:
            return "int64";
        case CameraKeySetting::Type::Float:
            return "float";
        case CameraKeySetting::Type::Double:
            return "double";
    }
    return "int32";
}

std::optional<CameraKeySetting::Type> parseType(const std::string& name) {
    if (name == "byte") return CameraKeySetting::Type::Byte;
    if (name == "int32") return CameraKeySetting::Type::Int32;
    if (name == "int64") return CameraKeySetting::Type::Int64;
    if (name == "float") return CameraKeySetting::Type::Float;
    if (name == "double") return CameraKeySetting::Type::Double;
    return std::nullopt;
}

void appendNumber(std::string& out, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    out += buf;
}

void appendLevels(std::string& out, const LevelOverride& l) {
    out += "{\"static\":";
    out += l.isStatic ? "true" : "false";
    out += ",\"black\":[";
    for (size_t i = 0; i < l.blackRggb.size(); ++i) {
        if (i) out += ',';
        appendNumber(out, l.blackRggb[i]);
    }
    out += "],\"white\":";
    appendNumber(out, l.white);
    out += '}';
}

void appendKeys(std::string& out, const std::vector<CameraKeySetting>& keys) {
    out += '[';
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        if (i) out += ',';
        out += "{\"tag\":";
        appendJsonString(out, k.tag);
        out += ",\"type\":\"";
        out += typeName(k.type);
        out += "\",\"scope\":\"";
        out += k.scope == CameraKeySetting::Scope::Session ? "session" : "request";
        out += "\",\"values\":[";
        for (size_t v = 0; v < k.values.size(); ++v) {
            if (v) out += ',';
            appendNumber(out, k.values[v]);
        }
        out += "],\"enabled\":";
        out += k.enabled ? "true" : "false";
        out += '}';
    }
    out += ']';
}

bool readLevels(const Json* j, LevelOverride& out) {
    if (!j || j->kind != Json::Kind::Object) return true;
    out.isStatic = j->flag("static", false);
    const auto& black = j->array("black");
    if (black.size() == 1) {
        out.blackRggb.fill(float(black[0].number));
    } else if (black.size() == 4) {
        for (size_t i = 0; i < 4; ++i) out.blackRggb[i] = float(black[i].number);
    } else if (out.isStatic) {
        return false;
    }
    out.white = float(j->num("white"));
    return !out.isStatic || out.white > 0.0f;
}

bool readKeys(const std::vector<Json>& list, std::vector<CameraKeySetting>& out, std::string* error) {
    for (const auto& k : list) {
        CameraKeySetting key;
        key.tag = k.str("tag");
        const auto type = parseType(k.str("type", "int32"));
        if (key.tag.empty() || !type) {
            if (error) *error = "invalid key " + key.tag;
            return false;
        }
        key.type = *type;
        key.scope = k.str("scope", "session") == "request" ? CameraKeySetting::Scope::Request
                                                           : CameraKeySetting::Scope::Session;
        for (const auto& v : k.array("values"))
            if (v.kind == Json::Kind::Number) key.values.push_back(v.number);
        key.enabled = k.flag("enabled", true);
        out.push_back(std::move(key));
    }
    return true;
}

}  // namespace

void appendJsonString(std::string& out, const std::string& value) {
    out += '"';
    for (const unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

std::string serializeCameraProfile(const CameraProfile& profile) {
    std::string out = "{\"id\":";
    appendJsonString(out, profile.id);
    out += ",\"lenses\":[";
    for (size_t i = 0; i < profile.lenses.size(); ++i) {
        const auto& r = profile.lenses[i];
        if (i) out += ',';
        out += "{\"id\":";
        appendJsonString(out, r.lensId);
        out += ",\"cameraId\":";
        appendJsonString(out, r.cameraId);
        out += ",\"physicalCameraId\":";
        appendJsonString(out, r.physicalCameraId);
        out += ",\"stream\":{\"format\":\"";
        out += geometry::rawPixelFormatName(r.preferredStream.format);
        out += "\",\"width\":" + std::to_string(r.preferredStream.width) +
               ",\"height\":" + std::to_string(r.preferredStream.height) + "},\"levels\":";
        appendLevels(out, r.levels);
        out += ",\"keys\":";
        appendKeys(out, r.keys);
        out += '}';
    }
    out += "]}";
    return out;
}

namespace {

bool parseRoot(const std::string& json, Json& root, std::string* error) {
    Parser parser(json);
    if (parser.parse(root) && root.kind == Json::Kind::Object) return true;
    if (error) *error = parser.error.empty() ? "not an object" : parser.error;
    return false;
}

std::optional<CameraProfile> readProfile(const Json& root, std::string* error) {
    CameraProfile profile;
    profile.id = root.str("id", "user");
    for (const auto& l : root.array("lenses")) {
        LensRoute r;
        r.lensId = l.str("id");
        r.cameraId = l.str("cameraId");
        r.physicalCameraId = l.str("physicalCameraId");
        if (r.lensId.empty() || r.cameraId.empty()) {
            if (error) *error = "lens without id or cameraId";
            return std::nullopt;
        }
        if (routeForLens(profile, r.lensId)) {
            if (error) *error = "duplicate lens id " + r.lensId;
            return std::nullopt;
        }
        if (const Json* s = l.get("stream"); s && s->kind == Json::Kind::Object) {
            r.preferredStream.format =
                s->str("format") == "RAW10" ? geometry::RawPixelFormat::Raw10 : geometry::RawPixelFormat::Raw16;
            r.preferredStream.width = uint32_t(std::max(0.0, s->num("width")));
            r.preferredStream.height = uint32_t(std::max(0.0, s->num("height")));
        }
        if (!readLevels(l.get("levels"), r.levels)) {
            if (error) *error = "invalid levels for lens " + r.lensId;
            return std::nullopt;
        }
        if (!readKeys(l.array("keys"), r.keys, error)) return std::nullopt;
        profile.lenses.push_back(std::move(r));
    }
    if (profile.lenses.empty()) {
        if (error) *error = "profile has no lenses";
        return std::nullopt;
    }
    return profile;
}

std::vector<std::string> strings(const std::vector<Json>& list) {
    std::vector<std::string> out;
    for (const auto& v : list)
        if (v.kind == Json::Kind::String && !v.text.empty()) out.push_back(v.text);
    return out;
}

}  // namespace

std::optional<CameraProfile> parseCameraProfile(const std::string& json, std::string* error) {
    Json root;
    if (!parseRoot(json, root, error)) return std::nullopt;
    return readProfile(root, error);
}

std::optional<BuiltInProfile> parseBuiltInProfile(const std::string& json, std::string* error) {
    Json root;
    if (!parseRoot(json, root, error)) return std::nullopt;
    auto profile = readProfile(root, error);
    if (!profile) return std::nullopt;
    if (profile->id.empty() || profile->id == "user" || profile->id == "generic") {
        if (error) *error = "built-in profile needs its own id";
        return std::nullopt;
    }
    BuiltInProfile out;
    out.profile = std::move(*profile);
    out.name = root.str("name", out.profile.id);
    out.version = int(root.num("version", 1));
    if (const Json* m = root.get("match"); m && m->kind == Json::Kind::Object) {
        out.match.models = strings(m->array("models"));
        out.match.verifiedModels = strings(m->array("verifiedModels"));
        if (const Json* p = m->get("propertyPrefixes"); p && p->kind == Json::Kind::Object)
            for (size_t i = 0; i < p->keys.size(); ++i)
                if (p->items[i].kind == Json::Kind::String && !p->items[i].text.empty())
                    out.match.propertyPrefixes.emplace_back(p->keys[i], p->items[i].text);
    }
    if (out.match.models.empty() && out.match.propertyPrefixes.empty()) {
        if (error) *error = "built-in profile " + out.profile.id + " matches no device";
        return std::nullopt;
    }
    return out;
}

}  // namespace rawrcam::camera
