#include "RawAhbImporter.h"

#include <android/log.h>

#include <sstream>
#include <stdexcept>
#include <vector>
namespace rawrcam::vulkan {
namespace {
void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
uint32_t findMemoryType(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    throw std::runtime_error("No compatible Vulkan memory type");
}
const char* usageName(VkImageUsageFlags u) {
    if (u == VK_IMAGE_USAGE_STORAGE_BIT) return "STORAGE";
    if (u == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return "COMPUTE";
    if (u == VK_IMAGE_USAGE_SAMPLED_BIT) return "SAMPLED";
    if (u == VK_IMAGE_USAGE_TRANSFER_SRC_BIT) return "TRANSFER_SRC";
    return "MIXED";
}
bool isZeroCopyEvidenceUsage(VkImageUsageFlags u) {
    return u == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
           u == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
}
void logZeroCopyEvidence(VkImageUsageFlags u, const std::string& line) {
    if (isZeroCopyEvidenceUsage(u)) __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "%s", line.c_str());
}
}  // namespace
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
      << " vkFormat=VK_FORMAT_R16_UINT tiling=OPTIMAL usage=" << usageName(imageUsage) << " externalFeatures=0x"
      << std::hex << externalProps.externalMemoryProperties.externalMemoryFeatures << " requiredAhbUsage=0x"
      << usage.androidHardwareBufferUsage;
    if (diagnostic_) diagnostic_(d.str());
    logZeroCopyEvidence(imageUsage, d.str());
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("R16_UINT external AHB format query failed for ") + usageName(imageUsage));
    if (!(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        throw std::runtime_error("R16_UINT external AHB is not importable for selected diagnostic usage");
    if (usage.androidHardwareBufferUsage == 0)
        throw std::runtime_error("Vulkan returned zero AHardwareBuffer usage for selected RAW access path");
    return usage.androidHardwareBufferUsage;
}
ImportedRaw& RawAhbImporter::import(AHardwareBuffer* ahb, uint32_t rawWidth_, uint32_t rawHeight_,
                                    VkImageUsageFlags imageUsage, RawImportOptions options) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(ahb);
    auto it = imported_.find(key);
    if (it != imported_.end()) return it->second;
    if (imported_.size() >= 16u)
        throw std::runtime_error(
            "AHardwareBuffer identity count exceeded bounded cache (16); refusing unbounded descriptor/import growth");

    ImportedRaw r{};
    r.ahb = ahb;
    AHardwareBuffer_describe(ahb, &r.desc);
    if (r.desc.width != rawWidth_ || r.desc.height != rawHeight_)
        throw std::runtime_error("AHB dimensions do not match configured RAW geometry");

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

    if (isZeroCopyEvidenceUsage(imageUsage) && diagnostic_) {
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R16_UINT, &formatProps);
        std::ostringstream fmt;
        fmt << "RAW_GPU_FORMAT_PROPERTIES vkFormat=VK_FORMAT_R16_UINT"
            << " linearFeatures=0x" << std::hex << formatProps.linearTilingFeatures << " optimalFeatures=0x"
            << formatProps.optimalTilingFeatures << " bufferFeatures=0x" << formatProps.bufferFeatures;
        diagnostic_(fmt.str());
        logZeroCopyEvidence(imageUsage, fmt.str());

        const VkImageUsageFlags modes[] = {
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        for (const VkImageUsageFlags mode : modes) {
            VkPhysicalDeviceExternalImageFormatInfo qiExternal{};
            qiExternal.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
            qiExternal.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkPhysicalDeviceImageFormatInfo2 qi{};
            qi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
            qi.pNext = &qiExternal;
            qi.format = VK_FORMAT_R16_UINT;
            qi.type = VK_IMAGE_TYPE_2D;
            qi.tiling = VK_IMAGE_TILING_OPTIMAL;
            qi.usage = mode;
            VkAndroidHardwareBufferUsageANDROID qUsage{};
            qUsage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
            VkExternalImageFormatProperties qExternal{};
            qExternal.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            qExternal.pNext = &qUsage;
            VkImageFormatProperties2 qProps{};
            qProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            qProps.pNext = &qExternal;
            const VkResult qr = vkGetPhysicalDeviceImageFormatProperties2(physical_, &qi, &qProps);
            std::ostringstream line;
            line << "RAW_GPU_EXTERNAL_QUERY path=" << usageName(mode) << " result=" << static_cast<int>(qr)
                 << " requiredAhbUsage=0x" << std::hex << qUsage.androidHardwareBufferUsage << " externalFeatures=0x"
                 << qExternal.externalMemoryProperties.externalMemoryFeatures;
            if (qr == VK_SUCCESS) {
                line << " maxExtent=" << std::dec << qProps.imageFormatProperties.maxExtent.width << 'x'
                     << qProps.imageFormatProperties.maxExtent.height
                     << " maxMipLevels=" << qProps.imageFormatProperties.maxMipLevels
                     << " maxArrayLayers=" << qProps.imageFormatProperties.maxArrayLayers << " sampleCounts=0x"
                     << std::hex << qProps.imageFormatProperties.sampleCounts << " maxResourceSize=" << std::dec
                     << qProps.imageFormatProperties.maxResourceSize;
            }
            diagnostic_(line.str());
            logZeroCopyEvidence(imageUsage, line.str());
        }
    }

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
        << " requestedVkUsage=" << usageName(imageUsage) << " exactStorageQueryResult=" << std::dec
        << static_cast<int>(formatResult) << " externalFeatures=0x" << std::hex
        << externalProps.externalMemoryProperties.externalMemoryFeatures << " requiredAhbUsage=0x"
        << requiredUsage.androidHardwareBufferUsage << " missingRequiredUsage=0x" << missingUsage;
    if (diagnostic_) diagnostic_(cap.str());
    logZeroCopyEvidence(imageUsage, cap.str());

    if (diagnostic_ && isZeroCopyEvidenceUsage(imageUsage)) {
        const uint64_t tightRowBytes = static_cast<uint64_t>(r.desc.width) * sizeof(uint16_t);
        const uint64_t ahbRowBytes = static_cast<uint64_t>(r.desc.stride) * sizeof(uint16_t);
        const uint64_t tightPayloadBytes = tightRowBytes * static_cast<uint64_t>(r.desc.height);
        const uint64_t stridePayloadBytes = ahbRowBytes * static_cast<uint64_t>(r.desc.height);
        std::ostringstream geom;
        geom << "RAW_ZERO_COPY_GEOMETRY_EVIDENCE"
             << " dims=" << std::dec << r.desc.width << "x" << r.desc.height << " widthMod16=" << (r.desc.width % 16u)
             << " widthMod32=" << (r.desc.width % 32u) << " widthMod64=" << (r.desc.width % 64u)
             << " widthMod128=" << (r.desc.width % 128u) << " widthMod256=" << (r.desc.width % 256u)
             << " stridePixels=" << r.desc.stride
             << " strideMinusWidth=" << (r.desc.stride >= r.desc.width ? r.desc.stride - r.desc.width : 0u)
             << " tightRowBytes=" << tightRowBytes << " ahbRowBytes=" << ahbRowBytes
             << " tightPayloadBytes=" << tightPayloadBytes << " stridePayloadBytes=" << stridePayloadBytes
             << " allocationSize=" << hp.allocationSize << " allocationMinusTight="
             << (hp.allocationSize >= tightPayloadBytes ? hp.allocationSize - tightPayloadBytes : 0ull)
             << " allocationMinusStridePayload="
             << (hp.allocationSize >= stridePayloadBytes ? hp.allocationSize - stridePayloadBytes : 0ull)
             << " actualUsage=0x" << std::hex << r.desc.usage << " vkFormat=" << std::dec << static_cast<int>(fp.format)
             << " externalFormat=" << fp.externalFormat << " formatFeatures=0x" << std::hex << fp.formatFeatures
             << " ahbMemoryTypeBits=0x" << hp.memoryTypeBits;
        diagnostic_(geom.str());
        logZeroCopyEvidence(imageUsage, geom.str());
    }

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
    const uint32_t selectedMemoryType = findMemoryType(physical_, hp.memoryTypeBits, 0);
    vkCheck(vkCreateImage(device_, &ci, nullptr, &r.image), "create imported raw image");

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties);
    if (diagnostic_ && isZeroCopyEvidenceUsage(imageUsage)) {
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
        vkCheck(vkBindImageMemory(device_, r.image, r.memory, 0), "bind imported raw");
    } catch (const std::exception& e) {
        // Nothing is cached on failure, so release what this attempt created
        // and carry the AHB facts to logcat with the error.
        if (r.memory) vkFreeMemory(device_, r.memory, nullptr);
        vkDestroyImage(device_, r.image, nullptr);
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

    // One batched diagnostic. Production modes never
    // enter this block. Every direct path remains a dedicated VkImage
    // import of the SAME Camera2 AHB; the copy buffer is explicitly labeled
    // as a non-zero-copy fallback measurement.
    if (options.advancedCandidates) {
        const uint64_t totalRawBytes = static_cast<uint64_t>(rawWidth_) * rawHeight_ * sizeof(uint16_t);

        // A. Android external-format candidate. This is a genuinely separate
        // sampled-image path: VK_FORMAT_UNDEFINED + VkExternalFormatANDROID.
        r.externalFormat = fp.externalFormat;
        r.externalFormatFeatures = fp.formatFeatures;
        r.externalComponents = fp.samplerYcbcrConversionComponents;
        r.externalSuggestedModel = fp.suggestedYcbcrModel;
        r.externalSuggestedRange = fp.suggestedYcbcrRange;
        r.externalSuggestedX = fp.suggestedXChromaOffset;
        r.externalSuggestedY = fp.suggestedYChromaOffset;

        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures{};
        ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        VkPhysicalDeviceFeatures2 featureQuery{};
        featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        featureQuery.pNext = &ycbcrFeatures;
        vkGetPhysicalDeviceFeatures2(physical_, &featureQuery);

        std::ostringstream extPropsLog;
        extPropsLog << "RAW_ZERO_COPY_EXTERNAL_FORMAT_PROPERTIES"
                    << " externalFormat=" << std::dec << fp.externalFormat << " formatFeatures=0x" << std::hex
                    << fp.formatFeatures
                    << " sampled=" << ((fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "yes" : "no")
                    << " ycbcrFeature=" << (ycbcrFeatures.samplerYcbcrConversion ? "yes" : "no")
                    << " model=" << std::dec << static_cast<int>(fp.suggestedYcbcrModel)
                    << " range=" << static_cast<int>(fp.suggestedYcbcrRange)
                    << " xChroma=" << static_cast<int>(fp.suggestedXChromaOffset)
                    << " yChroma=" << static_cast<int>(fp.suggestedYChromaOffset)
                    << " components=" << static_cast<int>(fp.samplerYcbcrConversionComponents.r) << ','
                    << static_cast<int>(fp.samplerYcbcrConversionComponents.g) << ','
                    << static_cast<int>(fp.samplerYcbcrConversionComponents.b) << ','
                    << static_cast<int>(fp.samplerYcbcrConversionComponents.a);
        if (diagnostic_) diagnostic_(extPropsLog.str());
        logZeroCopyEvidence(imageUsage, extPropsLog.str());

        if (fp.externalFormat != 0 && ycbcrFeatures.samplerYcbcrConversion == VK_TRUE &&
            (fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0) {
            // VkExternalFormatANDROID is NOT a legal member of
            // VkPhysicalDeviceImageFormatInfo2::pNext. External-format image
            // creation limits are derived directly from the AHB-reported
            // externalFormat/formatFeatures by the Vulkan image-creation
            // rules, so do not manufacture an invalid capability query here.
            std::ostringstream extGateLog;
            extGateLog << "RAW_ZERO_COPY_EXTERNAL_FORMAT_GATE"
                       << " source=AHB_PROPERTIES"
                       << " externalFormat=" << std::dec << fp.externalFormat << " sampledFeature=yes"
                       << " ycbcrFeature=yes"
                       << " actualUsage=0x" << std::hex << r.desc.usage;
            if (diagnostic_) diagnostic_(extGateLog.str());
            logZeroCopyEvidence(imageUsage, extGateLog.str());

            VkExternalMemoryImageCreateInfo extMemoryInfo{};
            extMemoryInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            extMemoryInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkExternalFormatANDROID extCreateFormat{};
            extCreateFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
            extCreateFormat.pNext = &extMemoryInfo;
            extCreateFormat.externalFormat = fp.externalFormat;
            VkImageCreateInfo extCi{};
            extCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            extCi.pNext = &extCreateFormat;
            extCi.flags = VK_IMAGE_CREATE_ALIAS_BIT;
            extCi.imageType = VK_IMAGE_TYPE_2D;
            extCi.format = VK_FORMAT_UNDEFINED;
            extCi.extent = {rawWidth_, rawHeight_, 1};
            extCi.mipLevels = 1;
            extCi.arrayLayers = 1;
            extCi.samples = VK_SAMPLE_COUNT_1_BIT;
            extCi.tiling = VK_IMAGE_TILING_OPTIMAL;
            extCi.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            extCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            extCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkResult extResult = vkCreateImage(device_, &extCi, nullptr, &r.externalFormatImage);
            if (extResult == VK_SUCCESS) {
                VkImportAndroidHardwareBufferInfoANDROID extImport{};
                extImport.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
                extImport.buffer = ahb;
                VkMemoryDedicatedAllocateInfo extDedicated{};
                extDedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
                extDedicated.pNext = &extImport;
                extDedicated.image = r.externalFormatImage;
                VkMemoryAllocateInfo extMa{};
                extMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                extMa.pNext = &extDedicated;
                extMa.allocationSize = hp.allocationSize;
                extMa.memoryTypeIndex = selectedMemoryType;
                extResult = vkAllocateMemory(device_, &extMa, nullptr, &r.externalFormatMemory);
            }
            if (extResult == VK_SUCCESS)
                extResult = vkBindImageMemory(device_, r.externalFormatImage, r.externalFormatMemory, 0);
            if (extResult == VK_SUCCESS) {
                r.externalFormatCandidateAvailable = true;
                std::string okExt = "RAW_ZERO_COPY_EXTERNAL_FORMAT_IMPORT_SUCCESS";
                if (diagnostic_) diagnostic_(okExt);
                logZeroCopyEvidence(imageUsage, okExt);
            } else {
                if (r.externalFormatImage) {
                    vkDestroyImage(device_, r.externalFormatImage, nullptr);
                    r.externalFormatImage = VK_NULL_HANDLE;
                }
                if (r.externalFormatMemory) {
                    vkFreeMemory(device_, r.externalFormatMemory, nullptr);
                    r.externalFormatMemory = VK_NULL_HANDLE;
                }
                std::ostringstream failExt;
                failExt << "RAW_ZERO_COPY_EXTERNAL_FORMAT_IMPORT_UNAVAILABLE result=" << static_cast<int>(extResult);
                if (diagnostic_) diagnostic_(failExt.str());
                logZeroCopyEvidence(imageUsage, failExt.str());
            }
        } else {
            std::string unavailable = "RAW_ZERO_COPY_EXTERNAL_FORMAT_IMPORT_UNAVAILABLE reason=properties_gate";
            if (diagnostic_) diagnostic_(unavailable);
            logZeroCopyEvidence(imageUsage, unavailable);
        }

        // B. DRM-modifier capability/intersection gate. Linear-only results
        // are explicitly classified as NO_NEW_PATH because LINEAR AHB image
        // access was already falsified in fix52b.
        auto getDrmProps = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            vkGetDeviceProcAddr(device_, "vkGetImageDrmFormatModifierPropertiesEXT"));
        uint32_t modifierCount = 0;
        std::vector<VkDrmFormatModifierPropertiesEXT> modifiers;
        if (getDrmProps != nullptr) {
            VkDrmFormatModifierPropertiesListEXT modifierList{};
            modifierList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
            VkFormatProperties2 drmFormatProps{};
            drmFormatProps.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
            drmFormatProps.pNext = &modifierList;
            vkGetPhysicalDeviceFormatProperties2(physical_, VK_FORMAT_R16_UINT, &drmFormatProps);
            modifierCount = modifierList.drmFormatModifierCount;
            modifiers.resize(modifierCount);
            if (modifierCount > 0) {
                modifierList.pDrmFormatModifierProperties = modifiers.data();
                vkGetPhysicalDeviceFormatProperties2(physical_, VK_FORMAT_R16_UINT, &drmFormatProps);
            }
        }
        std::ostringstream drmCaps;
        drmCaps << "RAW_ZERO_COPY_DRM_CAPS extensionFunction=" << (getDrmProps ? "yes" : "no")
                << " modifierCount=" << modifierCount;
        if (diagnostic_) diagnostic_(drmCaps.str());
        logZeroCopyEvidence(imageUsage, drmCaps.str());

        bool foundNonLinear = false;
        bool importedNonLinear = false;
        if (getDrmProps) {
            for (const auto& modifier : modifiers) {
                // DRM_FORMAT_MOD_LINEAR is defined as zero by drm_fourcc.h.
                const bool nonLinear = modifier.drmFormatModifier != 0ull;
                std::ostringstream one;
                one << "RAW_ZERO_COPY_DRM_MODIFIER modifier=0x" << std::hex << modifier.drmFormatModifier
                    << " planes=" << std::dec << modifier.drmFormatModifierPlaneCount << " tilingFeatures=0x"
                    << std::hex << modifier.drmFormatModifierTilingFeatures
                    << " class=" << (nonLinear ? "NON_LINEAR" : "LINEAR_NO_NEW_PATH");
                if (diagnostic_) diagnostic_(one.str());
                logZeroCopyEvidence(imageUsage, one.str());
                if (!nonLinear || importedNonLinear) continue;
                foundNonLinear = true;

                VkImageUsageFlags drmUsage = 0;
                const auto f = modifier.drmFormatModifierTilingFeatures;
                if ((f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                    drmUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                else if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                    drmUsage = VK_IMAGE_USAGE_STORAGE_BIT;
                else if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                    drmUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
                if (drmUsage == 0) continue;

                VkPhysicalDeviceExternalImageFormatInfo drmExternal{};
                drmExternal.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
                drmExternal.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
                VkPhysicalDeviceImageDrmFormatModifierInfoEXT drmInfo{};
                drmInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
                drmInfo.pNext = &drmExternal;
                drmInfo.drmFormatModifier = modifier.drmFormatModifier;
                drmInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VkPhysicalDeviceImageFormatInfo2 drmImageInfo{};
                drmImageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
                drmImageInfo.pNext = &drmInfo;
                drmImageInfo.format = VK_FORMAT_R16_UINT;
                drmImageInfo.type = VK_IMAGE_TYPE_2D;
                drmImageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                drmImageInfo.usage = drmUsage;
                VkAndroidHardwareBufferUsageANDROID drmRequiredUsage{};
                drmRequiredUsage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
                VkExternalImageFormatProperties drmExternalProps{};
                drmExternalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
                drmExternalProps.pNext = &drmRequiredUsage;
                VkImageFormatProperties2 drmImageProps{};
                drmImageProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
                drmImageProps.pNext = &drmExternalProps;
                const VkResult drmQuery =
                    vkGetPhysicalDeviceImageFormatProperties2(physical_, &drmImageInfo, &drmImageProps);
                const uint64_t drmMissingUsage = drmRequiredUsage.androidHardwareBufferUsage & ~r.desc.usage;
                std::ostringstream gate;
                gate << "RAW_ZERO_COPY_DRM_INTERSECTION modifier=0x" << std::hex << modifier.drmFormatModifier
                     << " usage=0x" << drmUsage << " result=" << std::dec << static_cast<int>(drmQuery)
                     << " requiredAhbUsage=0x" << std::hex << drmRequiredUsage.androidHardwareBufferUsage
                     << " missingRequiredUsage=0x" << drmMissingUsage << " externalFeatures=0x"
                     << drmExternalProps.externalMemoryProperties.externalMemoryFeatures;
                if (diagnostic_) diagnostic_(gate.str());
                logZeroCopyEvidence(imageUsage, gate.str());

                if (drmQuery != VK_SUCCESS || drmMissingUsage != 0 ||
                    (drmExternalProps.externalMemoryProperties.externalMemoryFeatures &
                     VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0)
                    continue;

                VkExternalMemoryImageCreateInfo drmMemoryInfo{};
                drmMemoryInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
                drmMemoryInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
                const uint64_t oneModifier = modifier.drmFormatModifier;
                VkImageDrmFormatModifierListCreateInfoEXT drmList{};
                drmList.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
                drmList.pNext = &drmMemoryInfo;
                drmList.drmFormatModifierCount = 1;
                drmList.pDrmFormatModifiers = &oneModifier;
                VkImageCreateInfo drmCi{};
                drmCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                drmCi.pNext = &drmList;
                drmCi.flags = VK_IMAGE_CREATE_ALIAS_BIT;
                drmCi.imageType = VK_IMAGE_TYPE_2D;
                drmCi.format = VK_FORMAT_R16_UINT;
                drmCi.extent = {rawWidth_, rawHeight_, 1};
                drmCi.mipLevels = 1;
                drmCi.arrayLayers = 1;
                drmCi.samples = VK_SAMPLE_COUNT_1_BIT;
                drmCi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                drmCi.usage = drmUsage;
                drmCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                drmCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                VkResult drmResult = vkCreateImage(device_, &drmCi, nullptr, &r.drmImage);
                if (drmResult == VK_SUCCESS) {
                    VkImportAndroidHardwareBufferInfoANDROID drmImport{};
                    drmImport.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
                    drmImport.buffer = ahb;
                    VkMemoryDedicatedAllocateInfo drmDedicated{};
                    drmDedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
                    drmDedicated.pNext = &drmImport;
                    drmDedicated.image = r.drmImage;
                    VkMemoryAllocateInfo drmMa{};
                    drmMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    drmMa.pNext = &drmDedicated;
                    drmMa.allocationSize = hp.allocationSize;
                    drmMa.memoryTypeIndex = selectedMemoryType;
                    drmResult = vkAllocateMemory(device_, &drmMa, nullptr, &r.drmMemory);
                }
                if (drmResult == VK_SUCCESS) drmResult = vkBindImageMemory(device_, r.drmImage, r.drmMemory, 0);
                if (drmResult == VK_SUCCESS) {
                    VkImageViewCreateInfo drmVi{};
                    drmVi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    drmVi.image = r.drmImage;
                    drmVi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    drmVi.format = VK_FORMAT_R16_UINT;
                    drmVi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    drmResult = vkCreateImageView(device_, &drmVi, nullptr, &r.drmView);
                }
                if (drmResult == VK_SUCCESS) {
                    VkImageDrmFormatModifierPropertiesEXT actual{};
                    actual.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
                    drmResult = getDrmProps(device_, r.drmImage, &actual);
                    if (drmResult == VK_SUCCESS) {
                        r.drmCandidateAvailable = true;
                        r.drmUsage = drmUsage;
                        importedNonLinear = true;
                        std::ostringstream okDrm;
                        okDrm << "RAW_ZERO_COPY_DRM_IMPORT_SUCCESS requestedModifier=0x" << std::hex << oneModifier
                              << " actualModifier=0x" << actual.drmFormatModifier << " usage=0x" << drmUsage;
                        if (diagnostic_) diagnostic_(okDrm.str());
                        logZeroCopyEvidence(imageUsage, okDrm.str());
                    }
                }
                if (!importedNonLinear) {
                    if (r.drmView) {
                        vkDestroyImageView(device_, r.drmView, nullptr);
                        r.drmView = VK_NULL_HANDLE;
                    }
                    if (r.drmImage) {
                        vkDestroyImage(device_, r.drmImage, nullptr);
                        r.drmImage = VK_NULL_HANDLE;
                    }
                    if (r.drmMemory) {
                        vkFreeMemory(device_, r.drmMemory, nullptr);
                        r.drmMemory = VK_NULL_HANDLE;
                    }
                }
            }
        }
        if (!foundNonLinear) {
            std::string noPath = "RAW_ZERO_COPY_DRM_RESULT NO_NEW_NON_LINEAR_PATH";
            if (diagnostic_) diagnostic_(noPath);
            logZeroCopyEvidence(imageUsage, noPath);
        } else if (!importedNonLinear) {
            std::string noImport = "RAW_ZERO_COPY_DRM_RESULT NON_LINEAR_PRESENT_BUT_NOT_IMPORTABLE_FOR_AHB";
            if (diagnostic_) diagnostic_(noImport);
            logZeroCopyEvidence(imageUsage, noImport);
        }

        // C. App-owned tightly packed buffer destination for transfer-path
        // parity and fallback timing. This intentionally moves totalRawBytes.
        VkBufferCreateInfo copyBi{};
        copyBi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        copyBi.size = totalRawBytes;
        copyBi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        copyBi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult copyResult = vkCreateBuffer(device_, &copyBi, nullptr, &r.copyBuffer);
        if (copyResult == VK_SUCCESS) {
            VkMemoryRequirements copyMr{};
            vkGetBufferMemoryRequirements(device_, r.copyBuffer, &copyMr);
            VkMemoryAllocateInfo copyMa{};
            copyMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            copyMa.allocationSize = copyMr.size;
            copyMa.memoryTypeIndex =
                findMemoryType(physical_, copyMr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            copyResult = vkAllocateMemory(device_, &copyMa, nullptr, &r.copyBufferMemory);
        }
        if (copyResult == VK_SUCCESS) copyResult = vkBindBufferMemory(device_, r.copyBuffer, r.copyBufferMemory, 0);
        if (copyResult == VK_SUCCESS) {
            r.copyBufferAvailable = true;
            std::ostringstream copyOk;
            copyOk << "RAW_ZERO_COPY_COPY_BUFFER_READY bytes=" << totalRawBytes;
            if (diagnostic_) diagnostic_(copyOk.str());
            logZeroCopyEvidence(imageUsage, copyOk.str());
        } else {
            if (r.copyBuffer) {
                vkDestroyBuffer(device_, r.copyBuffer, nullptr);
                r.copyBuffer = VK_NULL_HANDLE;
            }
            if (r.copyBufferMemory) {
                vkFreeMemory(device_, r.copyBufferMemory, nullptr);
                r.copyBufferMemory = VK_NULL_HANDLE;
            }
            std::ostringstream copyFail;
            copyFail << "RAW_ZERO_COPY_COPY_BUFFER_UNAVAILABLE result=" << static_cast<int>(copyResult);
            if (diagnostic_) diagnostic_(copyFail.str());
            logZeroCopyEvidence(imageUsage, copyFail.str());
        }
    }

    // fix53: when the candidate itself is STORAGE-only, preserve an
    // independent TRANSFER_SRC import of the same AHB solely to populate
    // the known-good app-owned R16_UINT oracle. This keeps the candidate
    // import contract pure while retaining same-frame/full-frame truth.
    if (imageUsage == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        const VkImageUsageFlags refUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkPhysicalDeviceExternalImageFormatInfo refExternalInfo{};
        refExternalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        refExternalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkPhysicalDeviceImageFormatInfo2 refImageInfo{};
        refImageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        refImageInfo.pNext = &refExternalInfo;
        refImageInfo.format = VK_FORMAT_R16_UINT;
        refImageInfo.type = VK_IMAGE_TYPE_2D;
        refImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        refImageInfo.usage = refUsage;
        VkAndroidHardwareBufferUsageANDROID refRequiredUsage{};
        refRequiredUsage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
        VkExternalImageFormatProperties refExternalProps{};
        refExternalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        refExternalProps.pNext = &refRequiredUsage;
        VkImageFormatProperties2 refProps{};
        refProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        refProps.pNext = &refExternalProps;
        const VkResult refQuery = vkGetPhysicalDeviceImageFormatProperties2(physical_, &refImageInfo, &refProps);
        const uint64_t refMissingUsage = refRequiredUsage.androidHardwareBufferUsage & ~r.desc.usage;

        std::ostringstream refQueryLog;
        refQueryLog << "RAW_ZERO_COPY_REFERENCE_QUERY result=" << static_cast<int>(refQuery) << " requiredAhbUsage=0x"
                    << std::hex << refRequiredUsage.androidHardwareBufferUsage << " actualUsage=0x" << r.desc.usage
                    << " missingRequiredUsage=0x" << refMissingUsage << " externalFeatures=0x"
                    << refExternalProps.externalMemoryProperties.externalMemoryFeatures;
        if (diagnostic_) diagnostic_(refQueryLog.str());
        logZeroCopyEvidence(imageUsage, refQueryLog.str());

        if (refQuery != VK_SUCCESS ||
            !(refExternalProps.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            refMissingUsage != 0) {
            throw std::runtime_error("fix53 TRANSFER_SRC reference alias is unavailable for STORAGE-only candidate");
        }

        VkExternalMemoryImageCreateInfo refEm{};
        refEm.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        refEm.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo refCi{};
        refCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        refCi.pNext = &refEm;
        refCi.flags = VK_IMAGE_CREATE_ALIAS_BIT;
        refCi.imageType = VK_IMAGE_TYPE_2D;
        refCi.format = VK_FORMAT_R16_UINT;
        refCi.extent = {rawWidth_, rawHeight_, 1};
        refCi.mipLevels = 1;
        refCi.arrayLayers = 1;
        refCi.samples = VK_SAMPLE_COUNT_1_BIT;
        refCi.tiling = VK_IMAGE_TILING_OPTIMAL;
        refCi.usage = refUsage;
        refCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        refCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(device_, &refCi, nullptr, &r.referenceTransferImage), "create transfer reference image");

        VkImportAndroidHardwareBufferInfoANDROID refImport{};
        refImport.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        refImport.buffer = ahb;
        VkMemoryDedicatedAllocateInfo refDedicated{};
        refDedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        refDedicated.pNext = &refImport;
        refDedicated.image = r.referenceTransferImage;
        VkMemoryAllocateInfo refMa{};
        refMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        refMa.pNext = &refDedicated;
        refMa.allocationSize = hp.allocationSize;
        refMa.memoryTypeIndex = selectedMemoryType;
        vkCheck(vkAllocateMemory(device_, &refMa, nullptr, &r.referenceTransferMemory),
                "allocate transfer reference AHB memory");
        vkCheck(vkBindImageMemory(device_, r.referenceTransferImage, r.referenceTransferMemory, 0),
                "bind transfer reference image");

        VkImageViewCreateInfo refVi{};
        refVi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        refVi.image = r.referenceTransferImage;
        refVi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        refVi.format = VK_FORMAT_R16_UINT;
        refVi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(device_, &refVi, nullptr, &r.referenceTransferView), "transfer reference image view");
        r.referenceTransferAvailable = true;

        std::ostringstream refOk;
        refOk << "RAW_ZERO_COPY_REFERENCE_IMPORT_SUCCESS candidateVkUsage=COMPUTE referenceVkUsage=TRANSFER_SRC"
              << " dims=" << std::dec << rawWidth_ << "x" << rawHeight_;
        if (diagnostic_) diagnostic_(refOk.str());
        logZeroCopyEvidence(imageUsage, refOk.str());
    }

    // Diagnostic-only zero-copy candidate: import the SAME camera AHB into a
    // second VkImage using LINEAR tiling. This is only attempted for the
    // zero_copy_ab mixed-usage diagnostic. A successful import remains a
    // true zero-copy path: both VkImages reference the original camera AHB.
    //
    // We query support first and refuse to create/bind anything unless the
    // driver explicitly advertises this exact external-image contract and
    // the already-created AHB contains every required usage bit.
    if (imageUsage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        VkPhysicalDeviceExternalImageFormatInfo linExternalInfo{};
        linExternalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        linExternalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkPhysicalDeviceImageFormatInfo2 linInfo{};
        linInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        linInfo.pNext = &linExternalInfo;
        linInfo.format = VK_FORMAT_R16_UINT;
        linInfo.type = VK_IMAGE_TYPE_2D;
        linInfo.tiling = VK_IMAGE_TILING_LINEAR;
        linInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        VkAndroidHardwareBufferUsageANDROID linUsage{};
        linUsage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
        VkExternalImageFormatProperties linExternalProps{};
        linExternalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        linExternalProps.pNext = &linUsage;
        VkImageFormatProperties2 linProps{};
        linProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        linProps.pNext = &linExternalProps;
        const VkResult linQuery = vkGetPhysicalDeviceImageFormatProperties2(physical_, &linInfo, &linProps);
        const uint64_t linMissingUsage = linUsage.androidHardwareBufferUsage & ~r.desc.usage;
        std::ostringstream linQueryLog;
        linQueryLog << "RAW_ZERO_COPY_LINEAR_QUERY result=" << static_cast<int>(linQuery) << " requiredAhbUsage=0x"
                    << std::hex << linUsage.androidHardwareBufferUsage << " actualUsage=0x" << r.desc.usage
                    << " missingRequiredUsage=0x" << linMissingUsage << " externalFeatures=0x"
                    << linExternalProps.externalMemoryProperties.externalMemoryFeatures;
        if (linQuery == VK_SUCCESS) {
            linQueryLog << " maxExtent=" << std::dec << linProps.imageFormatProperties.maxExtent.width << 'x'
                        << linProps.imageFormatProperties.maxExtent.height;
        }
        if (diagnostic_) diagnostic_(linQueryLog.str());
        logZeroCopyEvidence(imageUsage, linQueryLog.str());

        const bool linImportable = linQuery == VK_SUCCESS &&
                                   (linExternalProps.externalMemoryProperties.externalMemoryFeatures &
                                    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 &&
                                   linMissingUsage == 0;
        auto linearStage = [&](const char* stage) {
            std::ostringstream oss;
            oss << "RAW_ZERO_COPY_LINEAR_STAGE stage=" << stage;
            if (diagnostic_) diagnostic_(oss.str());
            logZeroCopyEvidence(imageUsage, oss.str());
        };
        if (linImportable) {
            VkExternalMemoryImageCreateInfo linEm{};
            linEm.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            linEm.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkImageCreateInfo linCi{};
            linCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            linCi.pNext = &linEm;
            linCi.flags = VK_IMAGE_CREATE_ALIAS_BIT;
            linCi.imageType = VK_IMAGE_TYPE_2D;
            linCi.format = VK_FORMAT_R16_UINT;
            linCi.extent = {rawWidth_, rawHeight_, 1};
            linCi.mipLevels = 1;
            linCi.arrayLayers = 1;
            linCi.samples = VK_SAMPLE_COUNT_1_BIT;
            linCi.tiling = VK_IMAGE_TILING_LINEAR;
            linCi.usage = VK_IMAGE_USAGE_STORAGE_BIT;
            linCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            linCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            linearStage("BEFORE_CREATE_IMAGE");
            VkResult linResult = vkCreateImage(device_, &linCi, nullptr, &r.linearImage);
            linearStage(linResult == VK_SUCCESS ? "AFTER_CREATE_IMAGE_OK" : "AFTER_CREATE_IMAGE_FAIL");
            if (linResult == VK_SUCCESS) {
                VkImportAndroidHardwareBufferInfoANDROID linImport{};
                linImport.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
                linImport.buffer = ahb;
                VkMemoryDedicatedAllocateInfo linDedicated{};
                linDedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
                linDedicated.pNext = &linImport;
                linDedicated.image = r.linearImage;
                VkMemoryAllocateInfo linMa{};
                linMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                linMa.pNext = &linDedicated;
                linMa.allocationSize = hp.allocationSize;
                linMa.memoryTypeIndex = selectedMemoryType;
                linearStage("BEFORE_ALLOCATE_MEMORY");
                linResult = vkAllocateMemory(device_, &linMa, nullptr, &r.linearMemory);
                linearStage(linResult == VK_SUCCESS ? "AFTER_ALLOCATE_MEMORY_OK" : "AFTER_ALLOCATE_MEMORY_FAIL");
            }
            if (linResult == VK_SUCCESS) {
                linearStage("BEFORE_BIND_IMAGE_MEMORY");
                linResult = vkBindImageMemory(device_, r.linearImage, r.linearMemory, 0);
                linearStage(linResult == VK_SUCCESS ? "AFTER_BIND_IMAGE_MEMORY_OK" : "AFTER_BIND_IMAGE_MEMORY_FAIL");
            }
            if (linResult == VK_SUCCESS) {
                VkImageViewCreateInfo linVi{};
                linVi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                linVi.image = r.linearImage;
                linVi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                linVi.format = VK_FORMAT_R16_UINT;
                linVi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                linearStage("BEFORE_CREATE_IMAGE_VIEW");
                linResult = vkCreateImageView(device_, &linVi, nullptr, &r.linearView);
                linearStage(linResult == VK_SUCCESS ? "AFTER_CREATE_IMAGE_VIEW_OK" : "AFTER_CREATE_IMAGE_VIEW_FAIL");
            }
            if (linResult == VK_SUCCESS) {
                r.linearCandidateAvailable = true;
                VkImageSubresource sub{};
                sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                VkSubresourceLayout layout{};
                linearStage("BEFORE_GET_SUBRESOURCE_LAYOUT");
                vkGetImageSubresourceLayout(device_, r.linearImage, &sub, &layout);
                linearStage("AFTER_GET_SUBRESOURCE_LAYOUT_OK");
                std::ostringstream okLin;
                okLin << "RAW_ZERO_COPY_LINEAR_IMPORT_SUCCESS rowPitch=" << std::dec << layout.rowPitch
                      << " offset=" << layout.offset << " size=" << layout.size << " arrayPitch=" << layout.arrayPitch
                      << " depthPitch=" << layout.depthPitch;
                if (diagnostic_) diagnostic_(okLin.str());
                logZeroCopyEvidence(imageUsage, okLin.str());
            } else {
                if (r.linearView) {
                    vkDestroyImageView(device_, r.linearView, nullptr);
                    r.linearView = VK_NULL_HANDLE;
                }
                if (r.linearImage) {
                    vkDestroyImage(device_, r.linearImage, nullptr);
                    r.linearImage = VK_NULL_HANDLE;
                }
                if (r.linearMemory) {
                    vkFreeMemory(device_, r.linearMemory, nullptr);
                    r.linearMemory = VK_NULL_HANDLE;
                }
                std::ostringstream failLin;
                failLin << "RAW_ZERO_COPY_LINEAR_IMPORT_UNAVAILABLE stageResult=" << static_cast<int>(linResult);
                if (diagnostic_) diagnostic_(failLin.str());
                logZeroCopyEvidence(imageUsage, failLin.str());
            }
        } else {
            std::string unavailable = "RAW_ZERO_COPY_LINEAR_IMPORT_UNAVAILABLE reason=query_or_usage";
            if (diagnostic_) diagnostic_(unavailable);
            logZeroCopyEvidence(imageUsage, unavailable);
        }
    }

    AHardwareBuffer_acquire(ahb);
    auto inserted = imported_.emplace(key, r);
    std::ostringstream ok;
    ok << "RAW_AHB_IMPORT_SUCCESS count=" << imported_.size() << " ptr=0x" << std::hex << key
       << " ahbFormat=" << std::dec << r.desc.format << " vkFormat=" << static_cast<int>(fp.format)
       << " stridePixels=" << r.desc.stride << " usage=0x" << std::hex << r.desc.usage
       << " importedVkUsage=" << usageName(imageUsage) << " queueOwnership=FOREIGN_EXT<->" << std::dec << queueFamily_;
    if (diagnostic_) diagnostic_(ok.str());
    logZeroCopyEvidence(imageUsage, ok.str());
    return inserted.first->second;
}

void RawAhbImporter::ensureBufferImport(AHardwareBuffer* ahb) noexcept {
    if (ahb == nullptr) return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(ahb);
    auto it = imported_.find(key);
    if (it == imported_.end()) {
        if (diagnostic_) diagnostic_("RAW_AHB_BUFFER_IMPORT_UNAVAILABLE reason=no_image_import_entry");
        return;
    }
    ImportedRaw& r = it->second;
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

        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = hp.allocationSize;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &bi, nullptr, &r.importBuffer), "create imported AHB buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, r.importBuffer, &req);
        if (req.size > hp.allocationSize) {
            vkDestroyBuffer(device_, r.importBuffer, nullptr);
            r.importBuffer = VK_NULL_HANDLE;
            throw std::runtime_error("imported buffer requirements exceed AHB allocation");
        }

        VkImportAndroidHardwareBufferInfoANDROID import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        import.buffer = ahb;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &import;
        dedicated.buffer = r.importBuffer;
        VkMemoryAllocateInfo ma{};
        ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ma.pNext = &dedicated;
        ma.allocationSize = hp.allocationSize;
        ma.memoryTypeIndex = findMemoryType(physical_, hp.memoryTypeBits, 0);
        vkCheck(vkAllocateMemory(device_, &ma, nullptr, &r.importBufferMemory), "allocate imported AHB buffer memory");
        vkCheck(vkBindBufferMemory(device_, r.importBuffer, r.importBufferMemory, 0), "bind imported AHB buffer");

        AHardwareBuffer_acquire(ahb);
        r.importBufferOwnsAcquire = true;
        r.importBufferStridePixels = desc.stride;
        r.importBufferAvailable = true;
        std::ostringstream ok;
        ok << "RAW_AHB_BUFFER_IMPORT_SUCCESS ptr=0x" << std::hex << key << " stridePixels=" << std::dec << desc.stride
           << " allocationSize=" << hp.allocationSize << " reqSize=" << req.size;
        if (diagnostic_) diagnostic_(ok.str());
    } catch (const std::exception& e) {
        if (r.importBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, r.importBufferMemory, nullptr);
            r.importBufferMemory = VK_NULL_HANDLE;
        }
        if (r.importBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, r.importBuffer, nullptr);
            r.importBuffer = VK_NULL_HANDLE;
        }
        r.importBufferAvailable = false;
        std::ostringstream fail;
        fail << "RAW_AHB_BUFFER_IMPORT_UNAVAILABLE reason=" << e.what();
        if (diagnostic_) diagnostic_(fail.str());
    }
}

ImportedRaw& RawAhbImporter::importRaw10Buffer(AHardwareBuffer* ahb, uint32_t expectedWidth, uint32_t expectedHeight,
                                               uint32_t rowStrideBytes) {
    if (ahb == nullptr) throw std::runtime_error("RAW10 buffer import: missing AHB");
    const uintptr_t key = reinterpret_cast<uintptr_t>(ahb);
    auto it = imported_.find(key);
    if (it == imported_.end()) {
        if (imported_.size() >= 16u)
            throw std::runtime_error(
                "AHardwareBuffer identity count exceeded bounded cache (16); refusing unbounded import growth");
        ImportedRaw r{};
        r.ahb = ahb;
        AHardwareBuffer_describe(ahb, &r.desc);
        if (r.desc.width != expectedWidth || r.desc.height != expectedHeight)
            throw std::runtime_error("AHB dimensions do not match configured RAW geometry");
        AHardwareBuffer_acquire(ahb);
        it = imported_.emplace(key, r).first;
    }
    ImportedRaw& r = it->second;
    r.gpuUnpack = true;
    r.rowStrideBytes = rowStrideBytes;
    ensureBufferImport(ahb);
    if (!r.importBufferAvailable) throw std::runtime_error("RAW10 buffer import unavailable");
    return r;
}

void RawAhbImporter::clear() noexcept {
    for (auto& kv : imported_) {
        auto& r = kv.second;
        if (r.drmView) vkDestroyImageView(device_, r.drmView, nullptr);
        if (r.drmImage) vkDestroyImage(device_, r.drmImage, nullptr);
        if (r.drmMemory) vkFreeMemory(device_, r.drmMemory, nullptr);
        if (r.externalFormatImage) vkDestroyImage(device_, r.externalFormatImage, nullptr);
        if (r.externalFormatMemory) vkFreeMemory(device_, r.externalFormatMemory, nullptr);
        if (r.copyBuffer) vkDestroyBuffer(device_, r.copyBuffer, nullptr);
        if (r.copyBufferMemory) vkFreeMemory(device_, r.copyBufferMemory, nullptr);
        if (r.importBuffer) vkDestroyBuffer(device_, r.importBuffer, nullptr);
        if (r.importBufferMemory) vkFreeMemory(device_, r.importBufferMemory, nullptr);
        if (r.linearView) vkDestroyImageView(device_, r.linearView, nullptr);
        if (r.referenceTransferView) vkDestroyImageView(device_, r.referenceTransferView, nullptr);
        if (r.referenceTransferImage) vkDestroyImage(device_, r.referenceTransferImage, nullptr);
        if (r.referenceTransferMemory) vkFreeMemory(device_, r.referenceTransferMemory, nullptr);
        if (r.linearImage) vkDestroyImage(device_, r.linearImage, nullptr);
        if (r.linearMemory) vkFreeMemory(device_, r.linearMemory, nullptr);
        if (r.view) vkDestroyImageView(device_, r.view, nullptr);
        if (r.image) vkDestroyImage(device_, r.image, nullptr);
        if (r.memory) vkFreeMemory(device_, r.memory, nullptr);
        if (r.ahb) AHardwareBuffer_release(r.ahb);
        if (r.importBufferOwnsAcquire && r.ahb) AHardwareBuffer_release(r.ahb);
    }
    imported_.clear();
}
}  // namespace rawrcam::vulkan
