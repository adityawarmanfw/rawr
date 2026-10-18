#include "RawAhbImporter.h"

#include <sstream>
#include <stdexcept>
#include <utility>

#include "vulkan/RawImportEvidence.h"
namespace rawrcam::vulkan {
namespace {
void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
}  // namespace
RawAhbImporter::AhbReference::AhbReference(AHardwareBuffer* ahb) noexcept : ahb_(ahb) {
    if (ahb_) AHardwareBuffer_acquire(ahb_);
}
RawAhbImporter::AhbReference::~AhbReference() {
    if (ahb_) AHardwareBuffer_release(ahb_);
}
RawAhbImporter::AhbReference::AhbReference(AhbReference&& other) noexcept
    : ahb_(std::exchange(other.ahb_, nullptr)) {}
RawAhbImporter::AhbReference& RawAhbImporter::AhbReference::operator=(AhbReference&& other) noexcept {
    if (this != &other) {
        if (ahb_) AHardwareBuffer_release(ahb_);
        ahb_ = std::exchange(other.ahb_, nullptr);
    }
    return *this;
}

RawAhbImporter::RawAhbImporter(VkPhysicalDevice p, VkDevice d, uint32_t q,
                               PFN_vkGetAndroidHardwareBufferPropertiesANDROID gp, Diagnostic diag)
    : physical_(p), device_(d), queueFamily_(q), getProperties_(gp), diagnostic_(std::move(diag)) {}
RawAhbImporter::~RawAhbImporter() { clear(); }
uint64_t RawAhbImporter::requiredHardwareBufferUsage(VkImageUsageFlags imageUsage) const {
    VkPhysicalDeviceExternalImageFormatInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkPhysicalDeviceImageFormatInfo2 imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    imageInfo.pNext = &externalInfo;
    imageInfo.format = VK_FORMAT_R16_UINT;
    imageInfo.type = VK_IMAGE_TYPE_2D;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = imageUsage;

    VkAndroidHardwareBufferUsageANDROID usage{};
    usage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
    VkExternalImageFormatProperties externalProps{};
    externalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    externalProps.pNext = &usage;
    VkImageFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    props.pNext = &externalProps;
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(physical_, &imageInfo, &props);
    std::ostringstream d;
    d << "RAW_READER_USAGE_QUERY result=" << static_cast<int>(result)
      << " vkFormat=VK_FORMAT_R16_UINT tiling=OPTIMAL usage=" << rawImportUsageName(imageUsage)
      << " externalFeatures=0x" << std::hex << externalProps.externalMemoryProperties.externalMemoryFeatures
      << " requiredAhbUsage=0x" << usage.androidHardwareBufferUsage;
    if (diagnostic_) diagnostic_(d.str());
    logZeroCopyEvidence(imageUsage, d.str());
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("R16_UINT external AHB format query failed for ") +
                                 rawImportUsageName(imageUsage));
    if (!(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        throw std::runtime_error("R16_UINT external AHB is not importable for selected diagnostic usage");
    if (usage.androidHardwareBufferUsage == 0)
        throw std::runtime_error("Vulkan returned zero AHardwareBuffer usage for selected RAW access path");
    return usage.androidHardwareBufferUsage;
}

RawAhbImporter::Entry RawAhbImporter::newEntry(AHardwareBuffer* ahb, uint32_t expectedWidth,
                                               uint32_t expectedHeight) const {
    if (imported_.size() >= 16u)
        throw std::runtime_error(
            "AHardwareBuffer identity count exceeded bounded cache (16); refusing unbounded import growth");
    Entry entry{AhbReference(ahb), AhbReference(), DeviceObjects(device_), ImportedRaw{}};
    entry.raw.ahb = ahb;
    AHardwareBuffer_describe(ahb, &entry.raw.desc);
    if (entry.raw.desc.width != expectedWidth || entry.raw.desc.height != expectedHeight)
        throw std::runtime_error("AHB dimensions do not match configured RAW geometry");
    return entry;
}

ImportedRaw& RawAhbImporter::import(AHardwareBuffer* ahb, uint32_t rawWidth_, uint32_t rawHeight_,
                                    VkImageUsageFlags imageUsage, RawImportOptions options) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(ahb);
    auto it = imported_.find(key);
    if (it != imported_.end()) {
        touch(key);
        return it->second.raw;
    }

    evictIdleEntry();
    Entry entry = newEntry(ahb, rawWidth_, rawHeight_);
    ImportedRaw& r = entry.raw;

    VkAndroidHardwareBufferFormatPropertiesANDROID fp{};
    fp.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID hp{};
    hp.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    hp.pNext = &fp;
    vkCheck(getProperties_(device_, ahb, &hp), "AHB properties");

    VkPhysicalDeviceExternalImageFormatInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkPhysicalDeviceImageFormatInfo2 imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    imageInfo.pNext = &externalInfo;
    imageInfo.format = VK_FORMAT_R16_UINT;
    imageInfo.type = VK_IMAGE_TYPE_2D;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = imageUsage;

    VkAndroidHardwareBufferUsageANDROID requiredUsage{};
    requiredUsage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
    VkExternalImageFormatProperties externalProps{};
    externalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    externalProps.pNext = &requiredUsage;
    VkImageFormatProperties2 imageProps{};
    imageProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    imageProps.pNext = &externalProps;
    const VkResult formatResult = vkGetPhysicalDeviceImageFormatProperties2(physical_, &imageInfo, &imageProps);

    const RawImportFacts facts{physical_, device_, ahb,  rawWidth_, rawHeight_, imageUsage, &r.desc, &hp, &fp,
                               &diagnostic_, options.advancedCandidates};
    const uint64_t missingUsage = requiredUsage.androidHardwareBufferUsage & ~r.desc.usage;
    std::ostringstream cap;
    cap << "RAW_AHB_CAPABILITY ptr=0x" << std::hex << key << " ahbFormat=" << std::dec << r.desc.format << "(0x"
        << std::hex << r.desc.format << ")"
        << " dims=" << std::dec << r.desc.width << "x" << r.desc.height << " layers=" << r.desc.layers
        << " stridePixels=" << r.desc.stride << " actualUsage=0x" << std::hex << r.desc.usage
        << " allocationSize=" << std::dec << hp.allocationSize << " memoryTypeBits=0x" << std::hex << hp.memoryTypeBits
        << " vkFormat=" << std::dec << static_cast<int>(fp.format)
        << " vkFormatIsR16Uint=" << (fp.format == VK_FORMAT_R16_UINT ? "yes" : "no")
        << " externalFormat=" << fp.externalFormat << " formatFeatures=0x" << std::hex << fp.formatFeatures
        << " storageFeature=" << ((fp.formatFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "yes" : "no")
        << " sampledFeature=" << ((fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "yes" : "no")
        << " requestedVkUsage=" << rawImportUsageName(imageUsage) << " exactStorageQueryResult=" << std::dec
        << static_cast<int>(formatResult) << " externalFeatures=0x" << std::hex
        << externalProps.externalMemoryProperties.externalMemoryFeatures << " requiredAhbUsage=0x"
        << requiredUsage.androidHardwareBufferUsage << " missingRequiredUsage=0x" << missingUsage;
    if (diagnostic_) diagnostic_(cap.str());
    logZeroCopyEvidence(imageUsage, cap.str());
    if (options.beforeImport) options.beforeImport(facts);

    if (fp.format != VK_FORMAT_R16_UINT) {
        std::ostringstream oss;
        oss << "direct RAW import unsupported: AHB Vulkan mapping is not VK_FORMAT_R16_UINT; vkFormat="
            << static_cast<int>(fp.format) << " externalFormat=" << fp.externalFormat;
        throw std::runtime_error(oss.str());
    }
    if ((imageUsage & VK_IMAGE_USAGE_STORAGE_BIT) && !(fp.formatFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        throw std::runtime_error("direct RAW import unsupported: mapped R16_UINT lacks STORAGE_IMAGE feature");
    if ((imageUsage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        throw std::runtime_error("RAW sampled diagnostic blocked: mapped R16_UINT lacks SAMPLED_IMAGE feature");
    if (formatResult != VK_SUCCESS)
        throw std::runtime_error("direct RAW import unsupported: exact R16_UINT STORAGE external-image query failed");
    if (!(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        throw std::runtime_error(
            "direct RAW import unsupported: R16_UINT STORAGE AHB external memory is not importable");
    if (missingUsage != 0) {
        std::ostringstream d;
        d << "RAW_AHB_USAGE_MISMATCH missingRequiredUsage=0x" << std::hex << missingUsage << " actual=0x"
          << r.desc.usage << " required=0x" << requiredUsage.androidHardwareBufferUsage;
        if (diagnostic_) diagnostic_(d.str());
    }

    VkExternalMemoryImageCreateInfo em{};
    em.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    em.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &em;
    if (isZeroCopyEvidenceUsage(imageUsage)) ci.flags = VK_IMAGE_CREATE_ALIAS_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16_UINT;
    ci.extent = {rawWidth_, rawHeight_, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = imageUsage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const uint32_t selectedMemoryType = findImportMemoryType(physical_, hp.memoryTypeBits, 0);
    vkCheck(vkCreateImage(device_, &ci, nullptr, &r.image), "create imported raw image");
    entry.objects.adoptImage(r.image);

    if (diagnostic_ && isZeroCopyEvidenceUsage(imageUsage)) {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties);
        std::ostringstream mem;
        mem << "RAW_GPU_IMPORT_MEMORY source=AHB_PROPERTIES"
            << " ahbAllocationSize=" << std::dec << hp.allocationSize << " ahbMemoryTypeBits=0x" << std::hex
            << hp.memoryTypeBits << " selectedMemoryType=" << std::dec << selectedMemoryType
            << " selectedMemoryFlags=0x" << std::hex << memoryProperties.memoryTypes[selectedMemoryType].propertyFlags;
        diagnostic_(mem.str());
        logZeroCopyEvidence(imageUsage, mem.str());
    }

    VkImportAndroidHardwareBufferInfoANDROID import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import;
    dedicated.image = r.image;
    VkMemoryAllocateInfo ma{};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.pNext = &dedicated;
    ma.allocationSize = hp.allocationSize;
    ma.memoryTypeIndex = selectedMemoryType;
    try {
        vkCheck(vkAllocateMemory(device_, &ma, nullptr, &r.memory), "allocate imported AHB memory");
        entry.objects.adoptMemory(r.memory);
        vkCheck(vkBindImageMemory(device_, r.image, r.memory, 0), "bind imported raw");
    } catch (const std::exception& e) {
        // Carry the AHB facts to logcat with the error; the entry releases what this attempt created.
        throw std::runtime_error(std::string(e.what()) + " | " + cap.str() +
                                 " selectedMemoryType=" + std::to_string(selectedMemoryType));
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = r.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16_UINT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCheck(vkCreateImageView(device_, &vi, nullptr, &r.view), "raw image view");
    entry.objects.adoptView(r.view);

    if (options.afterImport) options.afterImport(facts, r, entry.objects);

    touch(key);
    auto inserted = imported_.emplace(key, std::move(entry));
    const ImportedRaw& cached = inserted.first->second.raw;
    std::ostringstream ok;
    ok << "RAW_AHB_IMPORT_SUCCESS count=" << imported_.size() << " ptr=0x" << std::hex << key
       << " ahbFormat=" << std::dec << cached.desc.format << " vkFormat=" << static_cast<int>(fp.format)
       << " stridePixels=" << cached.desc.stride << " usage=0x" << std::hex << cached.desc.usage
       << " importedVkUsage=" << rawImportUsageName(imageUsage) << " queueOwnership=FOREIGN_EXT<->" << std::dec
       << queueFamily_;
    if (diagnostic_) diagnostic_(ok.str());
    logZeroCopyEvidence(imageUsage, ok.str());
    return inserted.first->second.raw;
}

void RawAhbImporter::ensureBufferImport(AHardwareBuffer* ahb) noexcept {
    if (ahb == nullptr) return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(ahb);
    auto it = imported_.find(key);
    if (it == imported_.end()) {
        if (diagnostic_) diagnostic_("RAW_AHB_BUFFER_IMPORT_UNAVAILABLE reason=no_image_import_entry");
        return;
    }
    Entry& entry = it->second;
    ImportedRaw& r = entry.raw;
    if (r.importBufferAttempted) return;
    r.importBufferAttempted = true;
    try {
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(ahb, &desc);
        if (desc.width == 0 || desc.height == 0 || desc.stride == 0) throw std::runtime_error("empty AHB description");

        VkAndroidHardwareBufferFormatPropertiesANDROID fp{};
        fp.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
        VkAndroidHardwareBufferPropertiesANDROID hp{};
        hp.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        hp.pNext = &fp;
        vkCheck(getProperties_(device_, ahb, &hp), "AHB buffer properties");
        if (hp.allocationSize == 0 || hp.memoryTypeBits == 0) throw std::runtime_error("empty AHB properties");

        // Deliberately not spec-shaped: on the Adreno 840 driver a buffer created
        // with VkExternalMemoryBufferCreateInfo reads camera AHBs back black, and
        // the plain buffer's memoryTypeBits exclude the AHB's type, so requiring
        // a type in both rejects every import (bisected 2026-10-13). Plain
        // buffer + AHB memory type reads the camera data correctly.
        DeviceObjects objects(device_);
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = hp.allocationSize;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        vkCheck(vkCreateBuffer(device_, &bi, nullptr, &buffer), "create imported AHB buffer");
        objects.adoptBuffer(buffer);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        if (req.size > hp.allocationSize) throw std::runtime_error("imported buffer requirements exceed AHB allocation");

        VkImportAndroidHardwareBufferInfoANDROID import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        import.buffer = ahb;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &import;
        dedicated.buffer = buffer;
        VkMemoryAllocateInfo ma{};
        ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ma.pNext = &dedicated;
        ma.allocationSize = hp.allocationSize;
        ma.memoryTypeIndex = findImportMemoryType(physical_, hp.memoryTypeBits, 0);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        vkCheck(vkAllocateMemory(device_, &ma, nullptr, &memory), "allocate imported AHB buffer memory");
        objects.adoptMemory(memory);
        vkCheck(vkBindBufferMemory(device_, buffer, memory, 0), "bind imported AHB buffer");

        entry.objects.append(std::move(objects));
        entry.bufferReference = AhbReference(ahb);
        r.importBuffer = buffer;
        r.importBufferStridePixels = desc.stride;
        r.importBufferAvailable = true;
        std::ostringstream ok;
        ok << "RAW_AHB_BUFFER_IMPORT_SUCCESS ptr=0x" << std::hex << key << " stridePixels=" << std::dec << desc.stride
           << " allocationSize=" << hp.allocationSize << " reqSize=" << req.size;
        if (diagnostic_) diagnostic_(ok.str());
    } catch (const std::exception& e) {
        r.importBufferAvailable = false;
        std::ostringstream fail;
        fail << "RAW_AHB_BUFFER_IMPORT_UNAVAILABLE reason=" << e.what();
        if (diagnostic_) diagnostic_(fail.str());
    }
}

ImportedRaw& RawAhbImporter::importRaw10Buffer(AHardwareBuffer* ahb, uint32_t expectedWidth, uint32_t expectedHeight,
                                               uint32_t rowStrideBytes) {
    if (ahb == nullptr) throw std::runtime_error("RAW10 buffer import: missing AHB");
    ImportedRaw& r = bufferEntry(ahb, expectedWidth, expectedHeight);
    r.gpuUnpack = true;
    r.rowStrideBytes = rowStrideBytes;
    ensureBufferImport(ahb);
    if (!r.importBufferAvailable) throw std::runtime_error("RAW10 buffer import unavailable");
    return r;
}

ImportedRaw& RawAhbImporter::importRaw16Buffer(AHardwareBuffer* ahb, uint32_t expectedWidth,
                                               uint32_t expectedHeight) {
    if (ahb == nullptr) throw std::runtime_error("RAW16 buffer import: missing AHB");
    ImportedRaw& r = bufferEntry(ahb, expectedWidth, expectedHeight);
    r.gpuUnpack = true;
    ensureBufferImport(ahb);
    if (!r.importBufferAvailable) throw std::runtime_error("RAW16 buffer import unavailable");
    r.rowStrideBytes = r.importBufferStridePixels * static_cast<uint32_t>(sizeof(uint16_t));
    return r;
}

ImportedRaw& RawAhbImporter::bufferEntry(AHardwareBuffer* ahb, uint32_t expectedWidth, uint32_t expectedHeight) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(ahb);
    auto it = imported_.find(key);
    if (it == imported_.end()) {
        evictIdleEntry();
        it = imported_.emplace(key, newEntry(ahb, expectedWidth, expectedHeight)).first;
    }
    touch(key);
    return it->second.raw;
}

void RawAhbImporter::clear() noexcept {
    imported_.clear();
    lastUse_.clear();
}
void RawAhbImporter::touch(uintptr_t key) { lastUse_[key] = std::chrono::steady_clock::now(); }
void RawAhbImporter::evictIdleEntry() {
    if (imported_.size() < 16u) return;
    constexpr auto kIdle = std::chrono::seconds(1);  // far longer than any in-flight frame
    auto oldest = lastUse_.end();
    for (auto it = lastUse_.begin(); it != lastUse_.end(); ++it)
        if (oldest == lastUse_.end() || it->second < oldest->second) oldest = it;
    if (oldest == lastUse_.end() || std::chrono::steady_clock::now() - oldest->second < kIdle) return;
    const auto entry = imported_.find(oldest->first);
    if (entry != imported_.end()) {
        if (evictListener_) evictListener_(entry->second.raw);
        imported_.erase(entry);
    }
    lastUse_.erase(oldest);
    if (diagnostic_) diagnostic_("RAW_AHB_IMPORT_EVICTED remaining=" + std::to_string(imported_.size()));
}
}  // namespace rawrcam::vulkan
