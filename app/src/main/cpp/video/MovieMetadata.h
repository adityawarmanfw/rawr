#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rawrcam::video {
using MdtaEntries = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

inline std::vector<uint8_t> utf8Data(const std::string& value) {
    std::vector<uint8_t> data{0, 0, 0, 1, 0, 0, 0, 0};
    data.insert(data.end(), value.begin(), value.end());
    const uint32_t size = static_cast<uint32_t>(data.size() + 8);
    data.insert(data.begin(), {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size), 'd', 'a', 't', 'a'});
    return data;
}

inline std::vector<uint8_t> mdtaMeta(const MdtaEntries& entries) {
    using Bytes = std::vector<uint8_t>;
    const auto put32 = [](Bytes& out, uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
    };
    const auto appendBox = [&put32](Bytes& out, const char* type, const Bytes& payload) {
        put32(out, uint32_t(payload.size() + 8));
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), payload.begin(), payload.end());
    };
    Bytes handler(8, 0);
    handler.insert(handler.end(), {'m', 'd', 't', 'a'});
    handler.resize(25, 0);
    Bytes keys(4, 0), items;
    put32(keys, uint32_t(entries.size()));
    uint32_t index = 1;
    for (const auto& [key, value] : entries) {
        put32(keys, uint32_t(key.size() + 8));
        keys.insert(keys.end(), {'m', 'd', 't', 'a'});
        keys.insert(keys.end(), key.begin(), key.end());
        put32(items, uint32_t(value.size() + 8));
        put32(items, index++);
        items.insert(items.end(), value.begin(), value.end());
    }
    Bytes body, out;
    appendBox(body, "hdlr", handler);
    appendBox(body, "keys", keys);
    appendBox(body, "ilst", items);
    appendBox(out, "meta", body);
    return out;
}
}