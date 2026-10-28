#include "camera/CameraProfileJson.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>

namespace rawrcam::camera {
namespace {

// CfaPattern codes by name (0=RGGB, 1=GRBG, 2=GBRG, 3=BGGR).
constexpr const char* kCfaOrderNames[] = {"RGGB", "GRBG", "GBRG", "BGGR"};
const char* cfaOrderName(uint32_t code) { return code < 4 ? kCfaOrderNames[code] : "RGGB"; }
std::optional<uint32_t> cfaOrderFromName(const std::string& name) {
    for (uint32_t i = 0; i < 4; ++i)
        if (name == kCfaOrderNames[i]) return i;
    return std::nullopt;
}

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

void appendDistortion(std::string& out, const geometry::LensDistortionModel& d) {
    out += ",\"distortion\":{\"model\":\"rational\",\"width\":" + std::to_string(d.width) +
           ",\"height\":" + std::to_string(d.height) + ",\"intrinsic\":[";
    const double intrinsic[4] = {d.fx, d.fy, d.cx, d.cy};
    for (size_t i = 0; i < 4; ++i) {
        if (i) out += ',';
        appendNumber(out, intrinsic[i]);
    }
    out += "],\"coefficients\":[";
    for (size_t i = 0; i < d.k.size(); ++i) {
        if (i) out += ',';
        appendNumber(out, d.k[i]);
    }
    out += "],\"outputScale\":";
    appendNumber(out, d.outputScale);
    out += '}';
}

// "distortion":{"model":"rational","width":4096,"height":3072,"intrinsic":[fx,fy,cx,cy],
//   "coefficients":[k1,k2,p1,p2,k3,k4,k5,k6],"outputScale":0.9758} (OpenCV rational model).
bool readDistortion(const Json* d, std::optional<geometry::LensDistortionModel>& out) {
    if (!d) return true;
    if (d->kind != Json::Kind::Object || d->str("model", "rational") != "rational") return false;
    geometry::LensDistortionModel m;
    m.width = uint32_t(std::max(0.0, d->num("width")));
    m.height = uint32_t(std::max(0.0, d->num("height")));
    const auto& intrinsic = d->array("intrinsic");
    const auto& coefficients = d->array("coefficients");
    if (intrinsic.size() != 4 || coefficients.size() != m.k.size()) return false;
    for (const auto* list : {&intrinsic, &coefficients})
        for (const auto& v : *list)
            if (v.kind != Json::Kind::Number) return false;
    m.fx = intrinsic[0].number;
    m.fy = intrinsic[1].number;
    m.cx = intrinsic[2].number;
    m.cy = intrinsic[3].number;
    for (size_t i = 0; i < m.k.size(); ++i) m.k[i] = coefficients[i].number;
    m.outputScale = d->num("outputScale", 1.0);
    if (!geometry::isValid(m)) return false;
    out = m;
    return true;
}

// "dcp":{"path":"/data/.../dcp_profiles/<id>.dcp"}. An unreadable profile
// never rejects the camera profile: the lens opens with the camera's colour.
void readDcp(const Json* d, LensRoute& r) {
    if (!d || d->kind != Json::Kind::Object) return;
    r.dcpPath = d->str("path");
    if (r.dcpPath.empty()) return;
    auto profile = color::loadDcp(r.dcpPath, &r.dcpError);
    if (profile && !profile->embeddable()) {
        r.dcpError = "profile forbids embedding (ProfileEmbedPolicy 2)";
        profile.reset();
    }
    if (profile && !color::dcpColorCalibration(*profile)) {
        r.dcpError = "profile matrices are not invertible";
        profile.reset();
    }
    if (profile) r.dcp = std::make_shared<const color::DcpProfile>(std::move(*profile));
}

// "mirror":"horizontal" | "vertical" | "both"; absent for none.
const char* mirrorName(uint32_t mirror) {
    switch (mirror & geometry::kLensMirrorMask) {
        case geometry::kLensMirrorHorizontal: return "horizontal";
        case geometry::kLensMirrorVertical: return "vertical";
        case geometry::kLensMirrorMask: return "both";
        default: return "none";
    }
}

bool readMirror(const std::string& name, uint32_t& out) {
    if (name.empty() || name == "none") out = 0;
    else if (name == "horizontal") out = geometry::kLensMirrorHorizontal;
    else if (name == "vertical") out = geometry::kLensMirrorVertical;
    else if (name == "both") out = geometry::kLensMirrorMask;
    else return false;
    return true;
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
        out += k.scope == CameraKeySetting::Scope::Session            ? "session"
               : k.scope == CameraKeySetting::Scope::Request          ? "request"
               : k.scope == CameraKeySetting::Scope::Decision         ? "decision"
               : k.scope == CameraKeySetting::Scope::SessionAndRequest ? "sessionAndRequest"
                                                                      : "still";
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

void appendExposure(std::string& out, const ExposureOverride& e) {
    out += "{\"extended\":";
    out += e.extended ? "true" : "false";
    out += ",\"isoMax\":" + std::to_string(e.isoMax) + ",\"shutterMaxNs\":" + std::to_string(e.shutterMaxNs) + '}';
}

void readExposure(const Json* j, ExposureOverride& out) {
    if (!j || j->kind != Json::Kind::Object) return;
    out.extended = j->flag("extended", false);
    out.isoMax = int32_t(std::clamp(j->num("isoMax"), 0.0, double(kExtendedIsoMax)));
    out.shutterMaxNs = int64_t(std::clamp(j->num("shutterMaxNs"), 0.0, double(kExtendedShutterMaxNs)));
}

// Reader usage as a name or a hex string ("0x100003"): the JSON number type
// can't carry 64-bit usage bits exactly.
std::optional<uint64_t> parseReaderUsage(const std::string& text) {
    if (text == "gpuDataBuffer") return kReaderUsageGpuDataBuffer;
    if (text == "none") return 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        uint64_t value = 0;
        const char* begin = text.data() + 2;
        const char* end = text.data() + text.size();
        const auto parsed = std::from_chars(begin, end, value, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == end) return value;
    }
    return std::nullopt;
}

std::string readerUsageName(uint64_t usage) {
    if (usage == kReaderUsageGpuDataBuffer) return "gpuDataBuffer";
    if (usage == 0) return "none";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(usage));
    return buf;
}

std::optional<std::pair<uint32_t, uint32_t>> readSize(const Json* j) {
    if (!j || j->kind != Json::Kind::Object) return std::nullopt;
    const double w = j->num("width"), h = j->num("height");
    if (w < 1.0 || h < 1.0 || w > 65536.0 || h > 65536.0 || std::floor(w) != w || std::floor(h) != h) return std::nullopt;
    return std::pair<uint32_t, uint32_t>{uint32_t(w), uint32_t(h)};
}

void appendSize(std::string& out, const char* name, const std::pair<uint32_t, uint32_t>& size) {
    out += std::string(",\"") + name + "\":{\"width\":" + std::to_string(size.first) +
           ",\"height\":" + std::to_string(size.second) + "}";
}

bool readKeys(const std::vector<Json>& list, std::vector<CameraKeySetting>& out, std::string* error) {
    for (const auto& k : list) {
        CameraKeySetting key;
        key.tag = k.str("tag");
        const auto first = key.tag.find_first_not_of(" \t\r\n\f\v");
        key.tag = first == std::string::npos ? "" :
            key.tag.substr(first, key.tag.find_last_not_of(" \t\r\n\f\v") - first + 1);
        if (key.tag.find_first_of(" \t\r\n\f\v") != std::string::npos) {
            if (error) *error = "Key name cannot contain whitespace; enter type and value in their separate fields";
            return false;
        }
        const auto type = parseType(k.str("type", "int32"));
        if (key.tag.empty() || !type) {
            if (error) *error = "invalid key " + key.tag;
            return false;
        }
        key.type = *type;
        const std::string scope = k.str("scope", "session");
        key.scope = scope == "request"             ? CameraKeySetting::Scope::Request
                    : scope == "still"             ? CameraKeySetting::Scope::Still
                    : scope == "decision"          ? CameraKeySetting::Scope::Decision
                    : scope == "sessionAndRequest" ? CameraKeySetting::Scope::SessionAndRequest
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
               ",\"height\":" + std::to_string(r.preferredStream.height);
        if (r.unlistedStream) out += ",\"unlisted\":true";
        if (r.rawStreamUseCase) out += ",\"useCase\":" + std::to_string(r.rawStreamUseCase);
        out += "},\"levels\":";
        appendLevels(out, r.levels);
        if (r.viewfinderLevels) { out += ",\"viewfinderLevels\":"; appendLevels(out, *r.viewfinderLevels); }
        if (r.captureLevels) { out += ",\"captureLevels\":"; appendLevels(out, *r.captureLevels); }
        out += ",\"exposure\":";
        appendExposure(out, r.exposure);
        out += ",\"keys\":";
        appendKeys(out, r.keys);
        if (r.readerUsage) out += ",\"readerUsage\":\"" + readerUsageName(*r.readerUsage) + "\"";
        if (r.captureReaderUsage)
            out += ",\"captureReaderUsage\":\"" + readerUsageName(*r.captureReaderUsage) + "\"";
        if (r.stillTemplate) out += *r.stillTemplate ? ",\"template\":\"still\"" : ",\"template\":\"preview\"";
        if (r.fixedFps > 0) out += ",\"fps\":" + std::to_string(r.fixedFps);
        if (r.disableCompanion) out += ",\"disableCompanion\":true";
        if (r.companionSize || r.companionUsage || r.companionMaxImages || r.companionPhysical || r.companionDetach ||
            r.companionFallback != CompanionFallback::Base || r.companionPrivate || !r.companionTargeted ||
            r.companionStreamUseCase) {
            out += ",\"companion\":{";
            if (r.companionSize)
                out += "\"width\":" + std::to_string(r.companionSize->first) +
                       ",\"height\":" + std::to_string(r.companionSize->second) + ",";
            out += r.companionPrivate ? "\"format\":\"private\"" : "\"format\":\"yuv\"";
            out += r.companionTargeted ? ",\"targeted\":true" : ",\"targeted\":false";
            if (r.companionUsage) out += ",\"usage\":\"" + readerUsageName(*r.companionUsage) + "\"";
            if (r.companionMaxImages) out += ",\"maxImages\":" + std::to_string(r.companionMaxImages);
            if (r.companionPhysical) out += *r.companionPhysical ? ",\"physical\":true" : ",\"physical\":false";
            if (r.companionDetach) out += *r.companionDetach ? ",\"detach\":true" : ",\"detach\":false";
            if (r.companionFallback != CompanionFallback::Base)
                out += r.companionFallback == CompanionFallback::Nearest ? ",\"fallback\":\"nearest\"" : ",\"fallback\":\"none\"";
            if (r.companionStreamUseCase) out += ",\"useCase\":" + std::to_string(r.companionStreamUseCase);
            out += '}';
        }
        if (r.startupDelayMs) out += ",\"startupDelayMs\":" + std::to_string(r.startupDelayMs);
        if (r.stopRepeatingForStill) out += ",\"stopRepeatingForStill\":true";
        if (r.auxJpegSize) {
            appendSize(out, "auxJpeg", *r.auxJpegSize);
            if (r.auxJpegUsage || r.auxJpegStreamUseCase) {
                out.pop_back();
                if (r.auxJpegUsage) out += ",\"usage\":\"" + readerUsageName(*r.auxJpegUsage) + "\"";
                if (r.auxJpegStreamUseCase) out += ",\"useCase\":" + std::to_string(r.auxJpegStreamUseCase);
                out += '}';
            }
        }
        if (r.rawMaxImages || r.readerProbe || r.readerDemote) {
            out += ",\"reader\":{";
            bool first = true;
            const auto field = [&](const std::string& text) {
                if (!first) out += ',';
                first = false;
                out += text;
            };
            if (r.rawMaxImages) field("\"maxImages\":" + std::to_string(r.rawMaxImages));
            if (r.readerProbe) field("\"probe\":true");
            if (r.readerDemote) field("\"demote\":true");
            out += '}';
        }
        if (!r.vendorGate.empty()) {
            out += ",\"vendorGate\":";
            appendJsonString(out, r.vendorGate);
        }
        if (r.sessionParams != SessionParamsPolicy::Cadence || r.opMode) {
            out += ",\"session\":{";
            if (r.sessionParams != SessionParamsPolicy::Cadence)
                out += std::string("\"params\":\"") + sessionParamsName(r.sessionParams) + "\"";
            if (r.opMode) out += std::string(r.sessionParams != SessionParamsPolicy::Cadence ? "," : "") + "\"opMode\":" + std::to_string(r.opMode);
            out += '}';
        }
        if (r.backend != BackendChoice::Auto) out += std::string(",\"backend\":\"") + backendChoiceName(r.backend) + "\"";
        if (r.requestDefaults != RequestDefaults::Rawr)
            out += std::string(",\"request\":{\"defaults\":\"") + requestDefaultsName(r.requestDefaults) + "\"}";
        if (r.gateHold)
            out += *r.gateHold == GateHold::UntilClosed ? ",\"gateHold\":\"closed\"" : ",\"gateHold\":\"configured\"";
        if (r.quirks.any()) {
            out += ",\"quirks\":[";
            bool first = true;
            for (const auto& name : quirkNames(r.quirks)) {
                if (!first) out += ',';
                first = false;
                appendJsonString(out, name);
            }
            out += ']';
        }
        if (r.captureStream) {
            out += ",\"captureStream\":{\"format\":\"";
            out += geometry::rawPixelFormatName(r.captureStream->format);
            out += "\",\"width\":" + std::to_string(r.captureStream->width) +
                   ",\"height\":" + std::to_string(r.captureStream->height);
            if (r.captureStreamUseCase) out += ",\"useCase\":" + std::to_string(r.captureStreamUseCase);
            out += '}';
        }
        if (r.captureBurst) out += ",\"captureBurst\":" + std::to_string(r.captureBurst);
        if (r.captureBurstSequential) out += ",\"captureBurstSequential\":true";
        if (r.cfaBlock > 1 || r.cfaOrder) {
            out += ",\"cfa\":{\"block\":" + std::to_string(r.cfaBlock);
            if (r.cfaOrder) out += std::string(",\"order\":\"") + cfaOrderName(*r.cfaOrder) + "\"";
            out += std::string(",\"live\":\"") + (r.cfaLiveQuality == 0 ? "fast" : "standard") + "\"";
            out += std::string(",\"photo\":\"") + (r.cfaPhotoQuality == 2 ? "high" : "standard") + "\"";
            out += '}';
        }
        if (r.distortion) appendDistortion(out, *r.distortion);
        if (r.mirror) out += std::string(",\"mirror\":\"") + mirrorName(r.mirror) + "\"";
        if (!r.dcpPath.empty()) {
            out += ",\"dcp\":{\"path\":";
            appendJsonString(out, r.dcpPath);
            out += '}';
        }
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

// Adds the named quirks to `out`; an unknown or non-string entry is an error.
bool readQuirks(const std::vector<Json>& list, CameraQuirks& out, std::string* error, const std::string& where) {
    for (const auto& v : list) {
        if (v.kind != Json::Kind::String || !setQuirk(out, v.text)) {
            if (error) *error = "unknown quirk " + (v.kind == Json::Kind::String ? v.text : std::string("?")) + " for " + where;
            return false;
        }
    }
    return true;
}

// A stream use case: 0 (not set), a standard value (1 to 6) or a vendor one; a non-integer or negative number is an error.
bool readUseCase(const Json* object, int64_t& out, std::string* error, const std::string& where) {
    if (!object || !object->get("useCase")) return true;
    const double v = object->num("useCase");
    if (v < 0.0 || v > 9007199254740991.0 || std::floor(v) != v) {
        if (error) *error = "invalid useCase for " + where;
        return false;
    }
    out = int64_t(v);
    return true;
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
            r.unlistedStream = s->flag("unlisted", false);
            if (!readUseCase(s, r.rawStreamUseCase, error, "stream of lens " + r.lensId)) return std::nullopt;
        }
        for (const auto& [name, field] : {std::pair{"readerUsage", &r.readerUsage},
                                          std::pair{"captureReaderUsage", &r.captureReaderUsage}}) {
            const Json* u = l.get(name);
            if (!u || u->kind != Json::Kind::String) continue;
            *field = parseReaderUsage(u->text);
            if (!*field) {
                if (error) *error = std::string("unknown ") + name + " " + u->text + " for lens " + r.lensId;
                return std::nullopt;
            }
        }
        if (const std::string t = l.str("template"); !t.empty()) {
            if (t != "still" && t != "preview") {
                if (error) *error = "unknown template " + t + " for lens " + r.lensId;
                return std::nullopt;
            }
            r.stillTemplate = t == "still";
        }
        if (l.get("streamUseCase")) {
            if (error) *error = "streamUseCase is not a profile field; set useCase on the stream, captureStream, companion or auxJpeg of lens " + r.lensId;
            return std::nullopt;
        }
        r.startupDelayMs = int32_t(std::clamp(l.num("startupDelayMs"), 0.0, 5000.0));
        r.stopRepeatingForStill = l.flag("stopRepeatingForStill", false);
        r.fixedFps = int32_t(std::clamp(l.num("fps"), 0.0, 240.0));
        if (const Json* disabled = l.get("disableCompanion")) {
            if (disabled->kind != Json::Kind::Bool || (disabled->boolean && l.get("companion"))) {
                if (error) *error = "disableCompanion must be boolean and cannot disable a required companion for lens " + r.lensId;
                return std::nullopt;
            }
            r.disableCompanion = disabled->boolean;
        }
        if (const Json* c = l.get("companion")) {
            const auto format = c->str("format", "yuv");
            if (format != "yuv" && format != "private") {
                if (error) *error = "invalid companion format for lens " + r.lensId;
                return std::nullopt;
            }
            r.companionPrivate = format == "private";
            r.companionTargeted = c->flag("targeted", true);
            // No width and height: an automatic size, with this block only tuning the companion the app adds.
            if (c->get("width") || c->get("height")) {
                r.companionSize = readSize(c);
                if (!r.companionSize) {
                    if (error) *error = "invalid companion for lens " + r.lensId;
                    return std::nullopt;
                }
            }
            if (const std::string usage = c->str("usage"); !usage.empty()) {
                r.companionUsage = parseReaderUsage(usage);
                if (!r.companionUsage) {
                    if (error) *error = "unknown companion usage " + usage + " for lens " + r.lensId;
                    return std::nullopt;
                }
            }
            if (c->get("maxImages")) {
                const double n = c->num("maxImages");
                if (n < 2.0 || n > 16.0 || std::floor(n) != n) {
                    if (error) *error = "companion maxImages must be 2 to 16 for lens " + r.lensId;
                    return std::nullopt;
                }
                r.companionMaxImages = uint32_t(n);
            }
            for (const auto& [name, field] : {std::pair{"physical", &r.companionPhysical}, std::pair{"detach", &r.companionDetach}}) {
                if (const Json* v = c->get(name)) {
                    if (v->kind != Json::Kind::Bool) {
                        if (error) *error = std::string("companion ") + name + " must be boolean for lens " + r.lensId;
                        return std::nullopt;
                    }
                    *field = v->boolean;
                }
            }
            if (!readUseCase(c, r.companionStreamUseCase, error, "companion of lens " + r.lensId)) return std::nullopt;
            if (const std::string fallback = c->str("fallback"); !fallback.empty()) {
                if (fallback != "base" && fallback != "nearest" && fallback != "none") {
                    if (error) *error = "unknown companion fallback " + fallback + " for lens " + r.lensId;
                    return std::nullopt;
                }
                if (r.companionSize) {
                    if (error) *error = "companion fallback needs an automatic size for lens " + r.lensId;
                    return std::nullopt;
                }
                r.companionFallback = fallback == "nearest" ? CompanionFallback::Nearest
                                      : fallback == "none"  ? CompanionFallback::None
                                                            : CompanionFallback::Base;
            }
        }
        if (const Json* a = l.get("auxJpeg")) {
            r.auxJpegSize = readSize(a);
            if (!r.auxJpegSize) {
                if (error) *error = "invalid auxJpeg for lens " + r.lensId;
                return std::nullopt;
            }
            if (!readUseCase(a, r.auxJpegStreamUseCase, error, "auxJpeg of lens " + r.lensId)) return std::nullopt;
            if (const std::string usage = a->str("usage"); !usage.empty()) {
                r.auxJpegUsage = parseReaderUsage(usage);
                if (!r.auxJpegUsage) {
                    if (error) *error = "unknown auxJpeg usage " + usage + " for lens " + r.lensId;
                    return std::nullopt;
                }
            }
        }
        if (const Json* rd = l.get("reader"); rd && rd->kind == Json::Kind::Object) {
            if (rd->get("maxImages")) {
                const double n = rd->num("maxImages");
                if (n < 6.0 || n > 16.0 || std::floor(n) != n) {
                    if (error) *error = "reader maxImages must be 6 to 16 for lens " + r.lensId;
                    return std::nullopt;
                }
                r.rawMaxImages = uint32_t(n);
            }
            r.readerProbe = rd->flag("probe", false);
            r.readerDemote = rd->flag("demote", false);
        }
        r.vendorGate = l.str("vendorGate");
        if (const std::string backend = l.str("backend"); !backend.empty()) {
            if (backend != "auto" && backend != "ndk" && backend != "java") {
                if (error) *error = "unknown backend " + backend + " for lens " + r.lensId;
                return std::nullopt;
            }
            r.backend = backend == "ndk" ? BackendChoice::Ndk : backend == "java" ? BackendChoice::Java : BackendChoice::Auto;
        }
        if (const Json* s = l.get("session"); s && s->kind == Json::Kind::Object) {
            if (s->get("opMode")) {
                const double mode = s->num("opMode");
                // 1 is the constrained high-speed mode; 1 and 2 are not accepted as a vendor mode.
                if (mode < 0.0 || mode > 2147483647.0 || std::floor(mode) != mode || mode == 1.0 || mode == 2.0) {
                    if (error) *error = "invalid opMode for lens " + r.lensId;
                    return std::nullopt;
                }
                r.opMode = uint32_t(mode);
            }
            if (const std::string params = s->str("params"); !params.empty()) {
                if (params != "cadence" && params != "none" && params != "full") {
                    if (error) *error = "unknown session params " + params + " for lens " + r.lensId;
                    return std::nullopt;
                }
                r.sessionParams = params == "none"   ? SessionParamsPolicy::None
                                  : params == "full" ? SessionParamsPolicy::Full
                                                     : SessionParamsPolicy::Cadence;
            }
        }
        if (const Json* q = l.get("request"); q && q->kind == Json::Kind::Object) {
            if (const std::string defaults = q->str("defaults"); !defaults.empty()) {
                if (defaults != "rawr" && defaults != "minimal") {
                    if (error) *error = "unknown request defaults " + defaults + " for lens " + r.lensId;
                    return std::nullopt;
                }
                r.requestDefaults = defaults == "minimal" ? RequestDefaults::Minimal : RequestDefaults::Rawr;
            }
        }
        if (const std::string hold = l.str("gateHold"); !hold.empty()) {
            if ((hold != "configured" && hold != "closed") || r.vendorGate.empty()) {
                if (error)
                    *error = std::string(r.vendorGate.empty() ? "gateHold needs a vendorGate" : "unknown gateHold " + hold) +
                             " for lens " + r.lensId;
                return std::nullopt;
            }
            r.gateHold = hold == "closed" ? GateHold::UntilClosed : GateHold::UntilConfigured;
        }
        if (!readQuirks(l.array("quirks"), r.quirks, error, "lens " + r.lensId)) return std::nullopt;
        if (const Json* c = l.get("captureStream"); c && c->kind == Json::Kind::Object) {
            geometry::RawStreamPreference capture;
            capture.format =
                c->str("format") == "RAW10" ? geometry::RawPixelFormat::Raw10 : geometry::RawPixelFormat::Raw16;
            capture.width = uint32_t(std::max(0.0, c->num("width")));
            capture.height = uint32_t(std::max(0.0, c->num("height")));
            if (capture.width == 0 || capture.height == 0) {
                if (error) *error = "invalid captureStream for lens " + r.lensId;
                return std::nullopt;
            }
            if (!readUseCase(c, r.captureStreamUseCase, error, "captureStream of lens " + r.lensId)) return std::nullopt;
            r.captureStream = capture;
        }
        r.captureBurst = uint32_t(std::clamp(l.num("captureBurst"), 0.0, 30.0));
        r.captureBurstSequential = l.flag("captureBurstSequential", false);
        if (const Json* c = l.get("cfa"); c && c->kind == Json::Kind::Object) {
            const double block = c->num("block", 1.0);
            if (block != 1.0 && block != 2.0 && block != 4.0) {
                if (error) *error = "invalid cfa block for lens " + r.lensId;
                return std::nullopt;
            }
            r.cfaBlock = uint32_t(block);
            if (const std::string order = c->str("order"); !order.empty()) {
                r.cfaOrder = cfaOrderFromName(order);
                if (!r.cfaOrder) {
                    if (error) *error = "invalid cfa order " + order + " for lens " + r.lensId;
                    return std::nullopt;
                }
            }
            r.cfaLiveQuality = c->str("live", "standard") == "fast" ? 0u : 1u;
            r.cfaPhotoQuality = c->str("photo", "high") == "standard" ? 1u : 2u;
        }
        if (!readLevels(l.get("levels"), r.levels)) {
            if (error) *error = "invalid levels for lens " + r.lensId;
            return std::nullopt;
        }
        for (const auto& [name, field] : {std::pair{"viewfinderLevels", &r.viewfinderLevels},
                                         std::pair{"captureLevels", &r.captureLevels}}) {
            if (const auto* value = l.get(name)) {
                LevelOverride levels;
                if (value->kind != Json::Kind::Object || !readLevels(value, levels)) {
                    if (error) *error = std::string("invalid ") + name + " for lens " + r.lensId;
                    return std::nullopt;
                }
                *field = levels;
            }
        }
        if (!readDistortion(l.get("distortion"), r.distortion)) {
            if (error) *error = "invalid distortion for lens " + r.lensId;
            return std::nullopt;
        }
        if (!readMirror(l.str("mirror"), r.mirror)) {
            if (error) *error = "invalid mirror for lens " + r.lensId;
            return std::nullopt;
        }
        readDcp(l.get("dcp"), r);
        readExposure(l.get("exposure"), r.exposure);
        if (!readKeys(l.array("keys"), r.keys, error)) return std::nullopt;
        if (r.backend == BackendChoice::Ndk && needsJavaBackend(r)) {
            if (error) *error = "operating mode and stream use case need the Java backend for lens " + r.lensId;
            return std::nullopt;
        }
        if (r.sessionParams == SessionParamsPolicy::None) {
            // A session-scope key travels in the session parameters: without them it would be dropped silently.
            for (const auto& k : r.keys) {
                if (k.enabled && keyAppliesIn(k.scope, CameraKeySetting::Scope::Session)) {
                    if (error) *error = "session params none conflicts with session key " + k.tag + " for lens " + r.lensId;
                    return std::nullopt;
                }
            }
        }
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
    if (!readQuirks(root.array("quirks"), out.quirks, error, "profile " + out.profile.id)) return std::nullopt;
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
