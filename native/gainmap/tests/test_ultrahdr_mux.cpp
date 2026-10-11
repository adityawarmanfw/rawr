// Host round-trip conformance for gainmap::UltrahdrMux.
// Assembles an UltraHDR file from synthetic minimal JPEGs, then parses the
// container back: MPF offsets, GContainer XMP, hdrgm fields, ISO 21496-1 box.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gainmap/UltrahdrMux.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

// Minimal valid-structure JPEG: SOI JFIF DQT SOF0 DHT SOS <entropy> EOI.
std::vector<uint8_t> fakeJpeg(uint8_t sofId, size_t entropyBytes, uint8_t fill) {
    std::vector<uint8_t> j = {0xFF, 0xD8};
    const uint8_t jfif[] = {0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    j.insert(j.end(), jfif, jfif + sizeof(jfif));
    const uint8_t dqt[] = {0xFF, 0xDB, 0x00, 0x43, 0x00};
    j.insert(j.end(), dqt, dqt + sizeof(dqt));
    j.insert(j.end(), 64, 0x08);
    const uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x10, 0x01, 0x01, 0x11, 0x00};
    j.insert(j.end(), sof, sof + sizeof(sof));
    (void)sofId;
    const uint8_t dht[] = {0xFF, 0xC4, 0x00, 0x1F, 0x00};
    j.insert(j.end(), dht, dht + sizeof(dht));
    j.insert(j.end(), 28, 0x00);
    const uint8_t sos[] = {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00};
    j.insert(j.end(), sos, sos + sizeof(sos));
    j.insert(j.end(), entropyBytes, fill);
    j.push_back(0xFF);
    j.push_back(0xD9);
    return j;
}

struct Segment {
    uint8_t marker;
    size_t pos;    // offset of FF byte
    size_t total;  // marker(2) + length field + payload
    std::vector<uint8_t> payload;
};

// Parses top-level segments of one JPEG image starting at soi (stops after SOS payload? No:
// parses markers until SOS, then caller handles entropy). Returns segments before SOS.
std::vector<Segment> parseHeaders(const std::vector<uint8_t>& f, size_t soi) {
    std::vector<Segment> segs;
    if (f[soi] != 0xFF || f[soi + 1] != 0xD8) return segs;
    size_t pos = soi + 2;
    while (pos + 4 <= f.size()) {
        if (f[pos] != 0xFF) break;
        uint8_t m = f[pos + 1];
        if (m == 0xDA || m == 0xD9 || (m >= 0xD0 && m <= 0xD7)) {
            segs.push_back({m, pos, 2, {}});
            break;
        }
        size_t len = ((size_t)f[pos + 2] << 8) | f[pos + 3];
        if (len < 2 || pos + 2 + len > f.size()) break;
        Segment s{m, pos, 2 + len, {}};
        s.payload.assign(f.begin() + pos + 4, f.begin() + pos + 2 + len);
        segs.push_back(s);
        pos += 2 + len;
    }
    return segs;
}

bool contains(const std::vector<uint8_t>& h, const std::string& n) {
    if (n.empty()) return true;
    for (size_t i = 0; i + n.size() <= h.size(); ++i)
        if (std::memcmp(h.data() + i, n.data(), n.size()) == 0) return true;
    return false;
}
std::string payloadStr(const Segment& s) { return std::string((const char*)s.payload.data(), s.payload.size()); }

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3]; }
}  // namespace

int main() {
    gainmap::UltrahdrMuxParams params;  // defaults
    const std::vector<uint8_t> base = fakeJpeg(0, 256, 0x5A);
    const std::vector<uint8_t> map = fakeJpeg(1, 64, 0xA5);
    const uint8_t exifFake[] = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0};

    std::vector<uint8_t> file;
    gainmap::UltrahdrMuxResult r = gainmap::UltrahdrMux::assemble(
        base.data(), base.size(), map.data(), map.size(), exifFake, sizeof(exifFake), params, file);
    check(r.ok, ("assemble ok: " + r.error).c_str());
    if (!r.ok) return 1;

    // 1. File starts with SOI; first EOI lands exactly at primarySize.
    check(file[0] == 0xFF && file[1] == 0xD8, "file SOI");
    check(r.primarySize + 2 <= file.size(), "primary in bounds");
    check(file[r.primarySize - 2] == 0xFF && file[r.primarySize - 1] == 0xD9, "primary ends EOI");
    // 2. Secondary SOI at reported offset; file ends with EOI.
    check(file[r.secondaryOffset] == 0xFF && file[r.secondaryOffset + 1] == 0xD8, "secondary SOI at offset");
    check(file[file.size() - 2] == 0xFF && file[file.size() - 1] == 0xD9, "file ends EOI");
    check(r.secondarySize == file.size() - r.secondaryOffset, "secondary size matches tail");
    // Legacy reader view: primary prefix is self-contained.
    {
        std::vector<uint8_t> primary(file.begin(), file.begin() + r.primarySize);
        auto segs = parseHeaders(primary, 0);
        bool hasSos = false;
        for (auto& s : segs) hasSos |= (s.marker == 0xDA);
        check(hasSos, "legacy primary has SOS");
    }

    // 3. Primary headers: EXIF, XMP GContainer, ISO, MPF.
    auto psegs = parseHeaders(file, 0);
    const Segment* xmp = nullptr;
    const Segment* mpf = nullptr;
    const Segment* exif = nullptr;
    const Segment* iso = nullptr;
    for (auto& s : psegs) {
        if (s.marker == 0xE1 && s.payload.size() > 6 && std::memcmp(s.payload.data(), "Exif\0\0", 6) == 0) exif = &s;
        if (s.marker == 0xE1 && contains(s.payload, "http://ns.adobe.com/xap/1.0/")) xmp = &s;
        if (s.marker == 0xE2 && contains(s.payload, "MPF")) mpf = &s;
        if (s.marker == 0xE2 && contains(s.payload, "urn:iso:std:iso:ts:21496:-1")) iso = &s;
    }
    check(exif != nullptr, "primary EXIF passthrough");
    check(xmp != nullptr, "primary XMP present");
    check(mpf != nullptr, "primary MPF present");
    check(iso != nullptr, "primary ISO present");
    if (xmp) {
        std::string x = payloadStr(*xmp);
        check(x.find("hdrgm:Version=\"1.0\"") != std::string::npos, "xmp version id");
        check(x.find("Item:Semantic=\"Primary\"") != std::string::npos, "xmp primary item");
        check(x.find("Item:Semantic=\"GainMap\"") != std::string::npos, "xmp gainmap item");
        char len[64];
        std::snprintf(len, sizeof(len), "Item:Length=\"%zu\"", r.secondarySize);
        check(x.find(len) != std::string::npos, "xmp gainmap length matches secondary");
        // XMP/RDF spec + Adobe-toolkit readers (Photos): the packet wrapper
        // locates the packet, rdf:about anchors the description. Without
        // them the container is ignored and the file reads as SDR.
        check(x.find("<?xpacket begin=") != std::string::npos, "xmp packet begin wrapper");
        check(x.find("<?xpacket end=\"w\"?>") != std::string::npos, "xmp packet end wrapper");
        check(x.find("<rdf:Description rdf:about=\"\"") != std::string::npos, "xmp rdf:about present");
    }
    if (iso) {
        // Primary ISO = namespace + 4 zero version bytes.
        check(iso->payload.size() == 28 + 4, "primary ISO version-only size");
    }
    size_t mpfMarkerPos = 0;
    if (mpf) {
        mpfMarkerPos = mpf->pos;
        const uint8_t* p = mpf->payload.data();
        check(std::memcmp(p, "MPF\0", 4) == 0, "mpf sig");
        check(p[4] == 'M' && p[5] == 'M' && p[6] == 0 && p[7] == 0x2A, "mpf big-endian");
        check(be32(p + 8) == 8, "mpf ifd offset");
        check(be16(p + 12) == 3, "mpf 3 tags");
        // MPEntry offset field at payload+12+2+36-4? Recompute: entries start after
        // 2(count)+36(tags)+4(attrIFD)=p+54; offset field lives at p+12+2+12+12+8=p+46.
        const uint32_t entryOff = be32(p + 46);
        const uint8_t* e = p + 4 + entryOff;  // offsets exclude the 4-byte sig
        check(be32(e) == 0x030000u, "mpf primary attr");
        check(be32(e + 4) == r.primarySize, "mpf primary size");
        check(be32(e + 8) == 0, "mpf primary offset 0");
        check(be32(e + 16) == 0x000000u, "mpf map attr jpeg");
        check(be32(e + 20) == r.secondarySize, "mpf map size");
        const uint32_t mapOff = be32(e + 24);
        check(mpfMarkerPos + 8 + mapOff == r.secondaryOffset, "mpf map offset lands on SOI");
    }

    // 4. Secondary headers: XMP hdrgm + ISO box.
    auto ssegs = parseHeaders(file, r.secondaryOffset);
    const Segment* sxmp = nullptr;
    const Segment* siso = nullptr;
    for (auto& s : ssegs) {
        if (s.marker == 0xE1 && contains(s.payload, "hdrgm:")) sxmp = &s;
        if (s.marker == 0xE2 && contains(s.payload, "urn:iso:std:iso:ts:21496:-1")) siso = &s;
    }
    check(sxmp != nullptr, "secondary XMP present");
    check(siso != nullptr, "secondary ISO present");
    if (sxmp) {
        std::string x = payloadStr(*sxmp);
        check(x.find("hdrgm:GainMapMin=\"0\"") != std::string::npos, "xmp gainmap min (identity floor)");
        check(x.find("hdrgm:GainMapMax=\"4.7090998\"") != std::string::npos, "xmp gainmap max");
        check(x.find("hdrgm:Gamma=\"1\"") != std::string::npos, "xmp gamma");
        check(x.find("hdrgm:OffsetSDR=\"0.015625\"") != std::string::npos, "xmp offset sdr");
        check(x.find("hdrgm:OffsetHDR=\"0.015625\"") != std::string::npos, "xmp offset hdr");
        check(x.find("hdrgm:HDRCapacityMin=\"0\"") != std::string::npos, "xmp cap min");
        check(x.find("hdrgm:HDRCapacityMax=\"4.7090998\"") != std::string::npos, "xmp cap max");
        check(x.find("hdrgm:BaseRenditionIsHDR=\"False\"") != std::string::npos, "xmp base sdr");
        check(x.find("<?xpacket begin=") != std::string::npos, "xmp packet begin wrapper");
        check(x.find("<?xpacket end=\"w\"?>") != std::string::npos, "xmp packet end wrapper");
        check(x.find("<rdf:Description rdf:about=\"\"") != std::string::npos, "xmp rdf:about present");
    }
    if (siso) {
        const uint8_t* b = siso->payload.data() + 28;  // skip namespace
        const size_t n = siso->payload.size() - 28;
        check(n == 4 + 1 + 8 + 8 + 5 * 8, "iso box size single-channel");
        check(be16(b) == 0 && be16(b + 2) == 0, "iso versions");
        check(b[4] == 0x40, "iso flags single+baseCS");
        auto frac = [&](size_t o) { return (double)(int32_t)be32(b + o) / (double)be32(b + o + 4); };
        auto ufrac = [&](size_t o) { return (double)be32(b + o) / (double)be32(b + o + 4); };
        check(std::fabs(exp2(ufrac(5)) - 1.0) < 1e-6, "iso base headroom == capMin(1.0)");
        check(std::fabs(exp2(ufrac(13)) - exp2(4.7090998)) / exp2(4.7090998) < 1e-4, "iso alt headroom == capMax");
        check(std::fabs(frac(21) - 0.0) < 1e-9, "iso gainmap min (identity floor)");
        check(std::fabs(frac(29) - 4.7090998) < 1e-6, "iso gainmap max");
        check(std::fabs(ufrac(37) - 1.0) < 1e-9, "iso gamma");
        check(std::fabs(frac(45) - 0.015625) < 1e-9, "iso offset sdr");
        check(std::fabs(frac(53) - 0.015625) < 1e-9, "iso offset hdr");
    }

    // 5. Map entropy survived: tail after secondary headers == map scan data.
    {
        bool tailOk = false;
        for (auto& s : ssegs) {
            if (s.marker == 0xDA) {
                const size_t segLen = ((size_t)file[s.pos + 2] << 8) | file[s.pos + 3];
                tailOk = (s.pos + 2 + segLen + 64 + 2 == file.size());  // SOS seg + entropy + EOI
                break;
            }
        }
        check(tailOk, "map entropy + EOI intact");
    }

    // 6. Invalid inputs rejected.
    {
        std::vector<uint8_t> bad = {0xFF, 0xD8, 0x00};
        std::vector<uint8_t> out;
        auto rr = gainmap::UltrahdrMux::assemble(bad.data(), bad.size(), map.data(), map.size(), nullptr, 0, params,
                                                 out);
        check(!rr.ok, "truncated base rejected");
        gainmap::UltrahdrMuxParams bp = params;
        bp.gainMapMaxLog2 = bp.gainMapMinLog2;
        rr = gainmap::UltrahdrMux::assemble(base.data(), base.size(), map.data(), map.size(), nullptr, 0, bp, out);
        check(!rr.ok, "bad params rejected");
    }

    // 7. RGB image components share one metadata set, in both ISO and XMP.
    {
        gainmap::UltrahdrMuxParams mp = params;
        mp.multiChannel = true;
        std::vector<uint8_t> mfile;
        gainmap::UltrahdrMuxResult mr = gainmap::UltrahdrMux::assemble(
            base.data(), base.size(), map.data(), map.size(), exifFake, sizeof(exifFake), mp, mfile);
        check(mr.ok, ("multi assemble ok: " + mr.error).c_str());
        if (mr.ok) {
            check(mfile.size() == file.size(), "multi file same size (identical metadata)");
            auto mssegs = parseHeaders(mfile, mr.secondaryOffset);
            const Segment* msiso = nullptr;
            const Segment* msxmp = nullptr;
            for (auto& s : mssegs) {
                if (s.marker == 0xE2 && contains(s.payload, "urn:iso:std:iso:ts:21496:-1")) msiso = &s;
                if (s.marker == 0xE1 && contains(s.payload, "hdrgm:")) msxmp = &s;
            }
            check(msiso != nullptr, "multi secondary ISO present");
            if (msiso) {
                const uint8_t* b = msiso->payload.data() + 28;
                check(b[4] == 0x40, "RGB uses one shared ISO parameter set");
                check(msiso->payload.size() == 28 + 4 + 1 + 8 + 8 + 5 * 8, "multi ISO same size as single");
            }
            check(msxmp != nullptr, "multi secondary XMP present");
            if (msxmp) {
                check(payloadStr(*msxmp).find("hdrgm:GainMapMax=\"4.7090998\"") != std::string::npos,
                      "multi XMP stays single-valued");
            }
        }
    }

    if (failures == 0) std::printf("ultrahdr_mux_test ok file=%zu primary=%zu secondary=%zu\n", file.size(),
                                   r.primarySize, r.secondarySize);
    return failures == 0 ? 0 : 1;
}
