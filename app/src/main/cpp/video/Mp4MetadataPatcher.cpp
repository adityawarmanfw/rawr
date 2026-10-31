#include "video/Mp4MetadataPatcher.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "video/HevcLogMetadata.h"
#include "video/MovieMetadata.h"

namespace rawrcam::video {
namespace {
using Bytes = std::vector<uint8_t>;

constexpr uint64_t kMaxMoovBytes = 64ull << 20;
// Packed ISO-639-2 "und", as QuickTime user data strings expect.
constexpr uint16_t kUndeterminedLanguage = 0x55C4;

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
void put16(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
void put32(Bytes& out, uint32_t v) {
    put16(out, v >> 16);
    put16(out, v & 0xFFFFu);
}
void putType(Bytes& out, const char* type) { out.insert(out.end(), type, type + 4); }
void putBytes(Bytes& out, const Bytes& bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }
Bytes box(const char* type, const Bytes& payload) {
    Bytes out;
    put32(out, uint32_t(8 + payload.size()));
    putType(out, type);
    putBytes(out, payload);
    return out;
}

bool readAt(int fd, uint64_t offset, uint8_t* out, size_t size) {
    size_t done = 0;
    while (done < size) {
        const ssize_t n = pread(fd, out + done, size - done, off_t(offset + done));
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}
bool writeAt(int fd, uint64_t offset, const uint8_t* data, size_t size) {
    size_t done = 0;
    while (done < size) {
        const ssize_t n = pwrite(fd, data + done, size - done, off_t(offset + done));
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}

struct Box {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t header = 8;
    char type[5]{};
};

// Parses consecutive boxes in [begin, end) of an in-memory buffer.
bool parseChildren(const Bytes& data, size_t begin, size_t end, std::vector<Box>& out) {
    size_t pos = begin;
    while (pos < end) {
        if (end - pos < 8) return false;
        Box b{};
        b.offset = pos;
        b.size = be32(&data[pos]);
        std::memcpy(b.type, &data[pos + 4], 4);
        if (b.size == 1) {
            if (end - pos < 16) return false;
            b.size = be64(&data[pos + 8]);
            b.header = 16;
        } else if (b.size == 0) {
            b.size = end - pos;
        }
        if (b.size < b.header || b.size > end - pos) return false;
        out.push_back(b);
        pos += size_t(b.size);
    }
    return true;
}

Bytes quickTimeString(const char* type, const std::string& value) {
    Bytes payload;
    put16(payload, uint32_t(value.size()));
    put16(payload, kUndeterminedLanguage);
    payload.insert(payload.end(), value.begin(), value.end());
    return box(type, payload);
}

// mdta key name and the contents of its ilst item (normally one data box).
// Reads an existing moov/meta box written with an mdta handler (as
// MPEG4Writer does for com.android.version). Returns false for any other
// meta layout, which is then left untouched.
bool readMdta(const Bytes& file, const Box& meta, MdtaEntries& out) {
    std::vector<Box> parts;
    if (!parseChildren(file, size_t(meta.offset + meta.header), size_t(meta.offset + meta.size), parts)) return false;
    const Box* hdlr = nullptr;
    const Box* keys = nullptr;
    const Box* ilst = nullptr;
    for (const Box& part : parts) {
        if (std::memcmp(part.type, "hdlr", 4) == 0)
            hdlr = &part;
        else if (std::memcmp(part.type, "keys", 4) == 0)
            keys = &part;
        else if (std::memcmp(part.type, "ilst", 4) == 0)
            ilst = &part;
        else
            return false;
    }
    if (!hdlr || !keys || !ilst || hdlr->size < 20 || std::memcmp(&file[size_t(hdlr->offset + 16)], "mdta", 4) != 0 ||
        keys->size < 16)
        return false;
    const size_t keysEnd = size_t(keys->offset + keys->size);
    size_t pos = size_t(keys->offset + 12);
    const uint32_t count = be32(&file[pos]);
    pos += 4;
    std::vector<std::string> names;
    for (uint32_t i = 0; i < count; ++i) {
        if (keysEnd - pos < 8) return false;
        const uint32_t size = be32(&file[pos]);
        if (size < 8 || size > keysEnd - pos) return false;
        names.emplace_back(reinterpret_cast<const char*>(&file[pos + 8]), size - 8);
        pos += size;
    }
    std::vector<Box> items;
    if (!parseChildren(file, size_t(ilst->offset + 8), size_t(ilst->offset + ilst->size), items)) return false;
    for (const Box& item : items) {
        const uint32_t index = be32(reinterpret_cast<const uint8_t*>(item.type));
        if (index == 0 || index > names.size() || item.header != 8) return false;
        out.emplace_back(names[index - 1],
                         Bytes(file.begin() + long(item.offset + 8), file.begin() + long(item.offset + item.size)));
    }
    return true;
}

// Rebuild only the HEVC sample-description ancestors. Media data and its
// chunk offsets never move. CSD and any in-band SPSs are also patched before
// muxing; rechecking hvcC here prevents a muxer from restoring SDR signalling.
Bytes logHvcc(const uint8_t* data, size_t size) {
    if (size < 23 || data[0] != 1) throw std::runtime_error("malformed hvcC");
    Bytes out(data, data + 23);
    size_t pos = 23;
    unsigned sps = 0;
    for (unsigned array = 0; array < data[22]; ++array) {
        if (size - pos < 3) throw std::runtime_error("truncated hvcC array");
        const unsigned type = data[pos] & 63;
        const unsigned count = (unsigned(data[pos + 1]) << 8) | data[pos + 2];
        out.insert(out.end(), data + pos, data + pos + 3);
        pos += 3;
        for (unsigned i = 0; i < count; ++i) {
            if (size - pos < 2) throw std::runtime_error("truncated hvcC length");
            const size_t n = (size_t(data[pos]) << 8) | data[pos + 1];
            pos += 2;
            if (n < 2 || n > size - pos) throw std::runtime_error("truncated hvcC NAL");
            if (type == 33) {
                const Bytes nal = logHevcSps(data + pos, n);
                if (nal.size() > 65535) throw std::runtime_error("SPS too large");
                put16(out, uint32_t(nal.size()));
                putBytes(out, nal);
                ++sps;
            } else {
                put16(out, uint32_t(n));
                out.insert(out.end(), data + pos, data + pos + n);
            }
            pos += n;
        }
    }
    if (pos != size || !sps) throw std::runtime_error("hvcC missing SPS or trailing data");
    return out;
}
Bytes logSampleDescriptions(const Bytes& data, const Box& parent, unsigned& descriptions, unsigned depth = 0) {
    if (depth > 8) throw std::runtime_error("sample description nesting too deep");
    const bool video = std::memcmp(parent.type, "hvc1", 4) == 0 || std::memcmp(parent.type, "hev1", 4) == 0;
    const bool stsd = std::memcmp(parent.type, "stsd", 4) == 0;
    const size_t prefix = video ? 78 : stsd ? 8 : 0;
    const size_t begin = size_t(parent.offset + parent.header);
    const size_t end = size_t(parent.offset + parent.size);
    if (prefix > end - begin) throw std::runtime_error("truncated sample description");
    std::vector<Box> children;
    if (!parseChildren(data, begin + prefix, end, children)) throw std::runtime_error("malformed sample description");
    if (stsd && be32(data.data() + begin + 4) != children.size()) throw std::runtime_error("invalid stsd count");
    Bytes body(data.begin() + begin, data.begin() + begin + prefix);
    unsigned hvcc = 0;
    for (const auto& child : children) {
        const auto* p = data.data() + child.offset;
        if (video && std::memcmp(child.type, "colr", 4) == 0) continue;
        if (video && std::memcmp(child.type, "hvcC", 4) == 0) {
            putBytes(body, box("hvcC", logHvcc(p + child.header, size_t(child.size - child.header))));
            ++hvcc;
        } else if (std::memcmp(child.type, "mdia", 4) == 0 || std::memcmp(child.type, "minf", 4) == 0 ||
                   std::memcmp(child.type, "stbl", 4) == 0 || std::memcmp(child.type, "stsd", 4) == 0 ||
                   (stsd && (std::memcmp(child.type, "hvc1", 4) == 0 || std::memcmp(child.type, "hev1", 4) == 0))) {
            putBytes(body, logSampleDescriptions(data, child, descriptions, depth + 1));
        } else
            body.insert(body.end(), p, p + child.size);
    }
    if (video) {
        if (hvcc != 1) throw std::runtime_error("HEVC sample description missing unique hvcC");
        Bytes color;
        putType(color, "nclx");
        put16(color, 2);     // unspecified primaries
        put16(color, 2);     // unspecified transfer
        put16(color, 1);     // BT.709 matrix
        color.push_back(0);  // limited range + reserved bits
        putBytes(body, box("colr", color));
        ++descriptions;
    }
    return box(parent.type, body);
}

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
}  // namespace

bool patchMp4DeviceMetadata(int fd, const Mp4DeviceMetadata& metadata, std::string* error) {
    if (fd < 0) return fail(error, "invalid fd");
    if (!metadata.log && metadata.make.empty() && metadata.model.empty()) return fail(error, "no metadata to write");
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) return fail(error, "cannot stat MP4");
    const uint64_t fileSize = uint64_t(st.st_size);

    // Top-level scan: find moov, and refuse files whose boxes do not tile the
    // file exactly (a size-0 box would swallow an appended moov).
    uint64_t pos = 0;
    Box moov{};
    bool found = false;
    while (pos < fileSize) {
        uint8_t header[16];
        if (fileSize - pos < 8 || !readAt(fd, pos, header, 8)) return fail(error, "truncated box header");
        Box b{};
        b.offset = pos;
        b.size = be32(header);
        std::memcpy(b.type, header + 4, 4);
        if (b.size == 1) {
            if (fileSize - pos < 16 || !readAt(fd, pos + 8, header + 8, 8)) return fail(error, "truncated largesize");
            b.size = be64(header + 8);
            b.header = 16;
        } else if (b.size == 0) {
            if (std::memcmp(b.type, "moov", 4) != 0) return fail(error, "open-ended top-level box");
            b.size = fileSize - pos;
        }
        if (b.size < b.header || b.size > fileSize - pos) return fail(error, "top-level box overruns file");
        if (std::memcmp(b.type, "moov", 4) == 0) {
            if (found) return fail(error, "multiple moov boxes");
            moov = b;
            found = true;
        }
        pos += b.size;
    }
    if (!found) return fail(error, "moov not found");
    if (moov.size > kMaxMoovBytes) return fail(error, "moov too large");
    if (moov.header != 8) return fail(error, "64-bit moov is not supported");

    Bytes old(size_t(moov.size));
    if (!readAt(fd, moov.offset, old.data(), old.size())) return fail(error, "cannot read moov");
    std::vector<Box> children;
    if (!parseChildren(old, moov.header, old.size(), children)) return fail(error, "malformed moov");

    const std::pair<const char*, const std::string*> userData[] = {
        {"\xA9mak", &metadata.make}, {"\xA9mod", &metadata.model}, {"\xA9too", &metadata.software}};
    Bytes ours;
    for (const auto& [type, value] : userData)
        if (!value->empty()) putBytes(ours, quickTimeString(type, *value));

    MdtaEntries androidKeys;
    for (const auto& entry :
         std::vector<std::pair<std::string, std::string>>{{"com.rawr.render-profile", metadata.renderProfile},
                                                          {"com.rawr.color-gamut", metadata.gamut},
                                                          {"com.rawr.transfer", metadata.transfer},
                                                          {"com.rawr.bit-depth", metadata.bitDepth}}) {
        if (!entry.second.empty()) androidKeys.emplace_back(entry.first, utf8Data(entry.second));
    }
    if (!metadata.make.empty()) androidKeys.emplace_back("com.android.manufacturer", utf8Data(metadata.make));
    if (!metadata.model.empty()) androidKeys.emplace_back("com.android.model", utf8Data(metadata.model));
    const auto mergedMdta = [&](MdtaEntries existing) {
        MdtaEntries merged;
        for (auto& entry : existing) {
            bool replaced = false;
            for (const auto& ours : androidKeys) replaced |= entry.first == ours.first;
            if (!replaced) merged.push_back(std::move(entry));
        }
        merged.insert(merged.end(), androidKeys.begin(), androidKeys.end());
        return mdtaMeta(merged);
    };

    Bytes body;
    bool wroteUdta = false;
    bool hasMeta = false;
    unsigned logDescriptions = 0;
    for (const Box& child : children) {
        const uint8_t* start = old.data() + child.offset;
        if (metadata.log && std::memcmp(child.type, "trak", 4) == 0) {
            try {
                putBytes(body, logSampleDescriptions(old, child, logDescriptions));
            } catch (const std::exception& e) {
                return fail(error, e.what());
            }
            continue;
        }
        if (std::memcmp(child.type, "udta", 4) == 0 && !wroteUdta) {
            // Merge: keep existing entries except the ones we replace.
            std::vector<Box> entries;
            if (!parseChildren(old, size_t(child.offset + child.header), size_t(child.offset + child.size), entries))
                return fail(error, "malformed udta");
            Bytes merged;
            for (const Box& entry : entries) {
                bool replaced = false;
                for (const auto& [type, value] : userData)
                    replaced |= !value->empty() && std::memcmp(entry.type, type, 4) == 0;
                if (!replaced)
                    merged.insert(merged.end(), old.data() + entry.offset, old.data() + entry.offset + entry.size);
            }
            putBytes(merged, ours);
            putBytes(body, box("udta", merged));
            wroteUdta = true;
            continue;
        }
        if (std::memcmp(child.type, "meta", 4) == 0 && !hasMeta) {
            hasMeta = true;
            MdtaEntries existing;
            if (readMdta(old, child, existing)) {
                putBytes(body, mergedMdta(std::move(existing)));
                continue;
            }
        }
        body.insert(body.end(), start, start + child.size);
    }
    if (metadata.log && !logDescriptions) return fail(error, "LOG MP4 has no HEVC sample description");
    if (!wroteUdta) putBytes(body, box("udta", ours));
    // A moov-level meta box that is not mdta is left alone.
    if (!hasMeta) putBytes(body, mergedMdta({}));
    if (body.size() + 8 > UINT32_MAX) return fail(error, "patched moov too large");
    const Bytes patched = box("moov", body);

    const bool moovIsLast = moov.offset + moov.size == fileSize;
    if (moovIsLast) {
        // Write the new tail first and the header last, so an interruption
        // leaves the old moov size in place.
        if (!writeAt(fd, moov.offset + 8, patched.data() + 8, patched.size() - 8))
            return fail(error, "write patched moov");
        if (patched.size() < moov.size && ftruncate(fd, off_t(moov.offset + patched.size())) != 0)
            return fail(error, "truncate");
        if (!writeAt(fd, moov.offset, patched.data(), 8)) return fail(error, "write moov header");
    } else {
        if (!writeAt(fd, fileSize, patched.data(), patched.size())) return fail(error, "append patched moov");
        const uint8_t freeType[4] = {'f', 'r', 'e', 'e'};
        if (!writeAt(fd, moov.offset + 4, freeType, 4)) return fail(error, "retire old moov");
    }
    return true;
}

}  // namespace rawrcam::video
