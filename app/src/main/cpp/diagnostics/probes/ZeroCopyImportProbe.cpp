#include "diagnostics/probes/ZeroCopyImportProbe.h"

#include <sstream>
#include <stdexcept>
#include <vector>

#include "vulkan/DeviceObjects.h"
#include "vulkan/RawAhbImporter.h"
#include "vulkan/RawImportEvidence.h"
namespace rawrcam::diagnostics {
namespace {
using vulkan::DeviceObjects;
using vulkan::ImportedRaw;
using vulkan::logZeroCopyEvidence;
using vulkan::RawImportFacts;

void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
bool hasDiagnostic(const RawImportFacts& f) { return f.diagnostic && *f.diagnostic; }
void report(const RawImportFacts& f, const std::string& line) {
    if (hasDiagnostic(f)) (*f.diagnostic)(line);
    logZeroCopyEvidence(f.usage, line);
}
uint32_t importMemoryType(const RawImportFacts& f) {
    return vulkan::findImportMemoryType(f.physical, f.properties->memoryTypeBits, 0);
}
// Dedicated import of f.ahb's memory for image, adopted into objects.
VkResult importAhbMemory(const RawImportFacts& f, VkImage image, DeviceObjects& objects) {
    VkImportAndroidHardwareBufferInfoANDROID import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import.buffer = f.ahb;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import;
    dedicated.image = image;
    VkMemoryAllocateInfo ma{};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.pNext = &dedicated;
    ma.allocationSize = f.properties->allocationSize;
    ma.memoryTypeIndex = importMemoryType(f);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = vkAllocateMemory(f.device, &ma, nullptr, &memory);
    if (result == VK_SUCCESS) objects.adoptMemory(memory);
    if (result == VK_SUCCESS) result = vkBindImageMemory(f.device, image, memory, 0);
    return result;
}
VkResult createR16View(const RawImportFacts& f, VkImage image, DeviceObjects& objects, VkImageView* view) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16_UINT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkResult result = vkCreateImageView(f.device, &vi, nullptr, view);
    if (result == VK_SUCCESS) objects.adoptView(*view);
    return result;
}

// A. Android external-format candidate. This is a genuinely separate
// sampled-image path: VK_FORMAT_UNDEFINED + VkExternalFormatANDROID.
void importExternalFormatCandidate(const RawImportFacts& f, ImportedRaw& r, DeviceObjects& objects) {
    const auto& fp = *f.format;
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
    vkGetPhysicalDeviceFeatures2(f.physical, &featureQuery);

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
    report(f, extPropsLog.str());

    if (fp.externalFormat == 0 || ycbcrFeatures.samplerYcbcrConversion != VK_TRUE ||
        (fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        report(f, "RAW_ZERO_COPY_EXTERNAL_FORMAT_IMPORT_UNAVAILABLE reason=properties_gate");
        return;
    }
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
               << " actualUsage=0x" << std::hex << f.desc->usage;
    report(f, extGateLog.str());

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
    extCi.extent = {f.width, f.height, 1};
    extCi.mipLevels = 1;
    extCi.arrayLayers = 1;
    extCi.samples = VK_SAMPLE_COUNT_1_BIT;
    extCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    extCi.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    extCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    extCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    DeviceObjects candidate(f.device);
    VkImage image = VK_NULL_HANDLE;
    VkResult extResult = vkCreateImage(f.device, &extCi, nullptr, &image);
    if (extResult == VK_SUCCESS) candidate.adoptImage(image);
    if (extResult == VK_SUCCESS) extResult = importAhbMemory(f, image, candidate);
    if (extResult != VK_SUCCESS) {
        std::ostringstream failExt;
        failExt << "RAW_ZERO_COPY_EXTERNAL_FORMAT_IMPORT_UNAVAILABLE result=" << static_cast<int>(extResult);
        report(f, failExt.str());
        return;
    }
    objects.append(std::move(candidate));
    r.externalFormatImage = image;
    r.externalFormatCandidateAvailable = true;
    report(f, "RAW_ZERO_COPY_EXTERNAL_FORMAT_IMPORT_SUCCESS");
}

// B. DRM-modifier capability/intersection gate. Linear-only results
// are explicitly classified as NO_NEW_PATH because LINEAR AHB image
// access was already falsified in fix52b.
void importDrmCandidate(const RawImportFacts& f, ImportedRaw& r, DeviceObjects& objects) {
    auto getDrmProps = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
        vkGetDeviceProcAddr(f.device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    uint32_t modifierCount = 0;
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers;
    if (getDrmProps != nullptr) {
        VkDrmFormatModifierPropertiesListEXT modifierList{};
        modifierList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 drmFormatProps{};
        drmFormatProps.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        drmFormatProps.pNext = &modifierList;
        vkGetPhysicalDeviceFormatProperties2(f.physical, VK_FORMAT_R16_UINT, &drmFormatProps);
        modifierCount = modifierList.drmFormatModifierCount;
        modifiers.resize(modifierCount);
        if (modifierCount > 0) {
            modifierList.pDrmFormatModifierProperties = modifiers.data();
            vkGetPhysicalDeviceFormatProperties2(f.physical, VK_FORMAT_R16_UINT, &drmFormatProps);
        }
    }
    std::ostringstream drmCaps;
    drmCaps << "RAW_ZERO_COPY_DRM_CAPS extensionFunction=" << (getDrmProps ? "yes" : "no")
            << " modifierCount=" << modifierCount;
    report(f, drmCaps.str());

    bool foundNonLinear = false;
    bool importedNonLinear = false;
    if (getDrmProps) {
        for (const auto& modifier : modifiers) {
            // DRM_FORMAT_MOD_LINEAR is defined as zero by drm_fourcc.h.
            const bool nonLinear = modifier.drmFormatModifier != 0ull;
            std::ostringstream one;
            one << "RAW_ZERO_COPY_DRM_MODIFIER modifier=0x" << std::hex << modifier.drmFormatModifier
                << " planes=" << std::dec << modifier.drmFormatModifierPlaneCount << " tilingFeatures=0x" << std::hex
                << modifier.drmFormatModifierTilingFeatures
                << " class=" << (nonLinear ? "NON_LINEAR" : "LINEAR_NO_NEW_PATH");
            report(f, one.str());
            if (!nonLinear || importedNonLinear) continue;
            foundNonLinear = true;

            VkImageUsageFlags drmUsage = 0;
            const auto features = modifier.drmFormatModifierTilingFeatures;
            if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                drmUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            else if (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                drmUsage = VK_IMAGE_USAGE_STORAGE_BIT;
            else if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
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
            const VkResult drmQuery = vkGetPhysicalDeviceImageFormatProperties2(f.physical, &drmImageInfo, &drmImageProps);
            const uint64_t drmMissingUsage = drmRequiredUsage.androidHardwareBufferUsage & ~f.desc->usage;
            std::ostringstream gate;
            gate << "RAW_ZERO_COPY_DRM_INTERSECTION modifier=0x" << std::hex << modifier.drmFormatModifier
                 << " usage=0x" << drmUsage << " result=" << std::dec << static_cast<int>(drmQuery)
                 << " requiredAhbUsage=0x" << std::hex << drmRequiredUsage.androidHardwareBufferUsage
                 << " missingRequiredUsage=0x" << drmMissingUsage << " externalFeatures=0x"
                 << drmExternalProps.externalMemoryProperties.externalMemoryFeatures;
            report(f, gate.str());

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
            drmCi.extent = {f.width, f.height, 1};
            drmCi.mipLevels = 1;
            drmCi.arrayLayers = 1;
            drmCi.samples = VK_SAMPLE_COUNT_1_BIT;
            drmCi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            drmCi.usage = drmUsage;
            drmCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            drmCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            DeviceObjects candidate(f.device);
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkResult drmResult = vkCreateImage(f.device, &drmCi, nullptr, &image);
            if (drmResult == VK_SUCCESS) candidate.adoptImage(image);
            if (drmResult == VK_SUCCESS) drmResult = importAhbMemory(f, image, candidate);
            if (drmResult == VK_SUCCESS) drmResult = createR16View(f, image, candidate, &view);
            if (drmResult != VK_SUCCESS) continue;
            VkImageDrmFormatModifierPropertiesEXT actual{};
            actual.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
            if (getDrmProps(f.device, image, &actual) != VK_SUCCESS) continue;
            objects.append(std::move(candidate));
            r.drmImage = image;
            r.drmView = view;
            r.drmCandidateAvailable = true;
            r.drmUsage = drmUsage;
            importedNonLinear = true;
            std::ostringstream okDrm;
            okDrm << "RAW_ZERO_COPY_DRM_IMPORT_SUCCESS requestedModifier=0x" << std::hex << oneModifier
                  << " actualModifier=0x" << actual.drmFormatModifier << " usage=0x" << drmUsage;
            report(f, okDrm.str());
        }
    }
    if (!foundNonLinear)
        report(f, "RAW_ZERO_COPY_DRM_RESULT NO_NEW_NON_LINEAR_PATH");
    else if (!importedNonLinear)
        report(f, "RAW_ZERO_COPY_DRM_RESULT NON_LINEAR_PRESENT_BUT_NOT_IMPORTABLE_FOR_AHB");
}

// C. App-owned tightly packed buffer destination for transfer-path
// parity and fallback timing. This intentionally moves totalRawBytes.
void createCopyBuffer(const RawImportFacts& f, ImportedRaw& r, DeviceObjects& objects) {
    const uint64_t totalRawBytes = static_cast<uint64_t>(f.width) * f.height * sizeof(uint16_t);
    VkBufferCreateInfo copyBi{};
    copyBi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    copyBi.size = totalRawBytes;
    copyBi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    copyBi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    DeviceObjects candidate(f.device);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult copyResult = vkCreateBuffer(f.device, &copyBi, nullptr, &buffer);
    if (copyResult == VK_SUCCESS) candidate.adoptBuffer(buffer);
    if (copyResult == VK_SUCCESS) {
        VkMemoryRequirements copyMr{};
        vkGetBufferMemoryRequirements(f.device, buffer, &copyMr);
        VkMemoryAllocateInfo copyMa{};
        copyMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        copyMa.allocationSize = copyMr.size;
        copyMa.memoryTypeIndex =
            vulkan::findImportMemoryType(f.physical, copyMr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        copyResult = vkAllocateMemory(f.device, &copyMa, nullptr, &memory);
        if (copyResult == VK_SUCCESS) candidate.adoptMemory(memory);
    }
    if (copyResult == VK_SUCCESS) copyResult = vkBindBufferMemory(f.device, buffer, memory, 0);
    if (copyResult != VK_SUCCESS) {
        std::ostringstream copyFail;
        copyFail << "RAW_ZERO_COPY_COPY_BUFFER_UNAVAILABLE result=" << static_cast<int>(copyResult);
        report(f, copyFail.str());
        return;
    }
    objects.append(std::move(candidate));
    r.copyBuffer = buffer;
    r.copyBufferAvailable = true;
    std::ostringstream copyOk;
    copyOk << "RAW_ZERO_COPY_COPY_BUFFER_READY bytes=" << totalRawBytes;
    report(f, copyOk.str());
}

// fix53: when the candidate itself is STORAGE-only, preserve an
// independent TRANSFER_SRC import of the same AHB solely to populate
// the known-good app-owned R16_UINT oracle. This keeps the candidate
// import contract pure while retaining same-frame/full-frame truth.
void importTransferReference(const RawImportFacts& f, ImportedRaw& r, DeviceObjects& objects) {
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
    const VkResult refQuery = vkGetPhysicalDeviceImageFormatProperties2(f.physical, &refImageInfo, &refProps);
    const uint64_t refMissingUsage = refRequiredUsage.androidHardwareBufferUsage & ~f.desc->usage;

    std::ostringstream refQueryLog;
    refQueryLog << "RAW_ZERO_COPY_REFERENCE_QUERY result=" << static_cast<int>(refQuery) << " requiredAhbUsage=0x"
                << std::hex << refRequiredUsage.androidHardwareBufferUsage << " actualUsage=0x" << f.desc->usage
                << " missingRequiredUsage=0x" << refMissingUsage << " externalFeatures=0x"
                << refExternalProps.externalMemoryProperties.externalMemoryFeatures;
    report(f, refQueryLog.str());

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
    refCi.extent = {f.width, f.height, 1};
    refCi.mipLevels = 1;
    refCi.arrayLayers = 1;
    refCi.samples = VK_SAMPLE_COUNT_1_BIT;
    refCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    refCi.usage = refUsage;
    refCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    refCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    DeviceObjects candidate(f.device);
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    const VkResult created = vkCreateImage(f.device, &refCi, nullptr, &image);
    if (created == VK_SUCCESS) candidate.adoptImage(image);
    vkCheck(created, "create transfer reference image");

    VkImportAndroidHardwareBufferInfoANDROID refImport{};
    refImport.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    refImport.buffer = f.ahb;
    VkMemoryDedicatedAllocateInfo refDedicated{};
    refDedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    refDedicated.pNext = &refImport;
    refDedicated.image = image;
    VkMemoryAllocateInfo refMa{};
    refMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    refMa.pNext = &refDedicated;
    refMa.allocationSize = f.properties->allocationSize;
    refMa.memoryTypeIndex = importMemoryType(f);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult allocated = vkAllocateMemory(f.device, &refMa, nullptr, &memory);
    if (allocated == VK_SUCCESS) candidate.adoptMemory(memory);
    vkCheck(allocated, "allocate transfer reference AHB memory");
    vkCheck(vkBindImageMemory(f.device, image, memory, 0), "bind transfer reference image");
    vkCheck(createR16View(f, image, candidate, &view), "transfer reference image view");
    objects.append(std::move(candidate));
    r.referenceTransferImage = image;
    r.referenceTransferView = view;
    r.referenceTransferAvailable = true;

    std::ostringstream refOk;
    refOk << "RAW_ZERO_COPY_REFERENCE_IMPORT_SUCCESS candidateVkUsage=COMPUTE referenceVkUsage=TRANSFER_SRC"
          << " dims=" << std::dec << f.width << "x" << f.height;
    report(f, refOk.str());
}

// Diagnostic-only zero-copy candidate: import the SAME camera AHB into a
// second VkImage using LINEAR tiling. This is only attempted for the
// zero_copy_ab mixed-usage diagnostic. A successful import remains a
// true zero-copy path: both VkImages reference the original camera AHB.
//
// We query support first and refuse to create/bind anything unless the
// driver explicitly advertises this exact external-image contract and
// the already-created AHB contains every required usage bit.
void importLinearCandidate(const RawImportFacts& f, ImportedRaw& r, DeviceObjects& objects) {
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
    const VkResult linQuery = vkGetPhysicalDeviceImageFormatProperties2(f.physical, &linInfo, &linProps);
    const uint64_t linMissingUsage = linUsage.androidHardwareBufferUsage & ~f.desc->usage;
    std::ostringstream linQueryLog;
    linQueryLog << "RAW_ZERO_COPY_LINEAR_QUERY result=" << static_cast<int>(linQuery) << " requiredAhbUsage=0x"
                << std::hex << linUsage.androidHardwareBufferUsage << " actualUsage=0x" << f.desc->usage
                << " missingRequiredUsage=0x" << linMissingUsage << " externalFeatures=0x"
                << linExternalProps.externalMemoryProperties.externalMemoryFeatures;
    if (linQuery == VK_SUCCESS) {
        linQueryLog << " maxExtent=" << std::dec << linProps.imageFormatProperties.maxExtent.width << 'x'
                    << linProps.imageFormatProperties.maxExtent.height;
    }
    report(f, linQueryLog.str());

    const bool linImportable = linQuery == VK_SUCCESS &&
                               (linExternalProps.externalMemoryProperties.externalMemoryFeatures &
                                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 &&
                               linMissingUsage == 0;
    if (!linImportable) {
        report(f, "RAW_ZERO_COPY_LINEAR_IMPORT_UNAVAILABLE reason=query_or_usage");
        return;
    }
    auto linearStage = [&](const char* stage) { report(f, std::string("RAW_ZERO_COPY_LINEAR_STAGE stage=") + stage); };
    VkExternalMemoryImageCreateInfo linEm{};
    linEm.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    linEm.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo linCi{};
    linCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    linCi.pNext = &linEm;
    linCi.flags = VK_IMAGE_CREATE_ALIAS_BIT;
    linCi.imageType = VK_IMAGE_TYPE_2D;
    linCi.format = VK_FORMAT_R16_UINT;
    linCi.extent = {f.width, f.height, 1};
    linCi.mipLevels = 1;
    linCi.arrayLayers = 1;
    linCi.samples = VK_SAMPLE_COUNT_1_BIT;
    linCi.tiling = VK_IMAGE_TILING_LINEAR;
    linCi.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    linCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    linCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    DeviceObjects candidate(f.device);
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    linearStage("BEFORE_CREATE_IMAGE");
    VkResult linResult = vkCreateImage(f.device, &linCi, nullptr, &image);
    if (linResult == VK_SUCCESS) candidate.adoptImage(image);
    linearStage(linResult == VK_SUCCESS ? "AFTER_CREATE_IMAGE_OK" : "AFTER_CREATE_IMAGE_FAIL");
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (linResult == VK_SUCCESS) {
        VkImportAndroidHardwareBufferInfoANDROID linImport{};
        linImport.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        linImport.buffer = f.ahb;
        VkMemoryDedicatedAllocateInfo linDedicated{};
        linDedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        linDedicated.pNext = &linImport;
        linDedicated.image = image;
        VkMemoryAllocateInfo linMa{};
        linMa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        linMa.pNext = &linDedicated;
        linMa.allocationSize = f.properties->allocationSize;
        linMa.memoryTypeIndex = importMemoryType(f);
        linearStage("BEFORE_ALLOCATE_MEMORY");
        linResult = vkAllocateMemory(f.device, &linMa, nullptr, &memory);
        if (linResult == VK_SUCCESS) candidate.adoptMemory(memory);
        linearStage(linResult == VK_SUCCESS ? "AFTER_ALLOCATE_MEMORY_OK" : "AFTER_ALLOCATE_MEMORY_FAIL");
    }
    if (linResult == VK_SUCCESS) {
        linearStage("BEFORE_BIND_IMAGE_MEMORY");
        linResult = vkBindImageMemory(f.device, image, memory, 0);
        linearStage(linResult == VK_SUCCESS ? "AFTER_BIND_IMAGE_MEMORY_OK" : "AFTER_BIND_IMAGE_MEMORY_FAIL");
    }
    if (linResult == VK_SUCCESS) {
        linearStage("BEFORE_CREATE_IMAGE_VIEW");
        linResult = createR16View(f, image, candidate, &view);
        linearStage(linResult == VK_SUCCESS ? "AFTER_CREATE_IMAGE_VIEW_OK" : "AFTER_CREATE_IMAGE_VIEW_FAIL");
    }
    if (linResult != VK_SUCCESS) {
        std::ostringstream failLin;
        failLin << "RAW_ZERO_COPY_LINEAR_IMPORT_UNAVAILABLE stageResult=" << static_cast<int>(linResult);
        report(f, failLin.str());
        return;
    }
    objects.append(std::move(candidate));
    r.linearImage = image;
    r.linearView = view;
    r.linearCandidateAvailable = true;
    VkImageSubresource sub{};
    sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    linearStage("BEFORE_GET_SUBRESOURCE_LAYOUT");
    vkGetImageSubresourceLayout(f.device, image, &sub, &layout);
    linearStage("AFTER_GET_SUBRESOURCE_LAYOUT_OK");
    std::ostringstream okLin;
    okLin << "RAW_ZERO_COPY_LINEAR_IMPORT_SUCCESS rowPitch=" << std::dec << layout.rowPitch
          << " offset=" << layout.offset << " size=" << layout.size << " arrayPitch=" << layout.arrayPitch
          << " depthPitch=" << layout.depthPitch;
    report(f, okLin.str());
}
}  // namespace

void logZeroCopyImportEvidence(const RawImportFacts& f) {
    if (!vulkan::isZeroCopyEvidenceUsage(f.usage) || !hasDiagnostic(f)) return;
    VkFormatProperties formatProps{};
    vkGetPhysicalDeviceFormatProperties(f.physical, VK_FORMAT_R16_UINT, &formatProps);
    std::ostringstream fmt;
    fmt << "RAW_GPU_FORMAT_PROPERTIES vkFormat=VK_FORMAT_R16_UINT"
        << " linearFeatures=0x" << std::hex << formatProps.linearTilingFeatures << " optimalFeatures=0x"
        << formatProps.optimalTilingFeatures << " bufferFeatures=0x" << formatProps.bufferFeatures;
    report(f, fmt.str());

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
        const VkResult qr = vkGetPhysicalDeviceImageFormatProperties2(f.physical, &qi, &qProps);
        std::ostringstream line;
        line << "RAW_GPU_EXTERNAL_QUERY path=" << vulkan::rawImportUsageName(mode) << " result=" << static_cast<int>(qr)
             << " requiredAhbUsage=0x" << std::hex << qUsage.androidHardwareBufferUsage << " externalFeatures=0x"
             << qExternal.externalMemoryProperties.externalMemoryFeatures;
        if (qr == VK_SUCCESS) {
            line << " maxExtent=" << std::dec << qProps.imageFormatProperties.maxExtent.width << 'x'
                 << qProps.imageFormatProperties.maxExtent.height
                 << " maxMipLevels=" << qProps.imageFormatProperties.maxMipLevels
                 << " maxArrayLayers=" << qProps.imageFormatProperties.maxArrayLayers << " sampleCounts=0x" << std::hex
                 << qProps.imageFormatProperties.sampleCounts << " maxResourceSize=" << std::dec
                 << qProps.imageFormatProperties.maxResourceSize;
        }
        report(f, line.str());
    }

    const auto& desc = *f.desc;
    const auto& hp = *f.properties;
    const auto& fp = *f.format;
    const uint64_t tightRowBytes = static_cast<uint64_t>(desc.width) * sizeof(uint16_t);
    const uint64_t ahbRowBytes = static_cast<uint64_t>(desc.stride) * sizeof(uint16_t);
    const uint64_t tightPayloadBytes = tightRowBytes * static_cast<uint64_t>(desc.height);
    const uint64_t stridePayloadBytes = ahbRowBytes * static_cast<uint64_t>(desc.height);
    std::ostringstream geom;
    geom << "RAW_ZERO_COPY_GEOMETRY_EVIDENCE"
         << " dims=" << std::dec << desc.width << "x" << desc.height << " widthMod16=" << (desc.width % 16u)
         << " widthMod32=" << (desc.width % 32u) << " widthMod64=" << (desc.width % 64u)
         << " widthMod128=" << (desc.width % 128u) << " widthMod256=" << (desc.width % 256u)
         << " stridePixels=" << desc.stride
         << " strideMinusWidth=" << (desc.stride >= desc.width ? desc.stride - desc.width : 0u)
         << " tightRowBytes=" << tightRowBytes << " ahbRowBytes=" << ahbRowBytes
         << " tightPayloadBytes=" << tightPayloadBytes << " stridePayloadBytes=" << stridePayloadBytes
         << " allocationSize=" << hp.allocationSize << " allocationMinusTight="
         << (hp.allocationSize >= tightPayloadBytes ? hp.allocationSize - tightPayloadBytes : 0ull)
         << " allocationMinusStridePayload="
         << (hp.allocationSize >= stridePayloadBytes ? hp.allocationSize - stridePayloadBytes : 0ull)
         << " actualUsage=0x" << std::hex << desc.usage << " vkFormat=" << std::dec << static_cast<int>(fp.format)
         << " externalFormat=" << fp.externalFormat << " formatFeatures=0x" << std::hex << fp.formatFeatures
         << " ahbMemoryTypeBits=0x" << hp.memoryTypeBits;
    report(f, geom.str());
}

void importZeroCopyCandidates(const RawImportFacts& f, ImportedRaw& raw, DeviceObjects& objects) {
    // One batched diagnostic. Every direct path remains a dedicated VkImage
    // import of the SAME Camera2 AHB; the copy buffer is explicitly labeled
    // as a non-zero-copy fallback measurement.
    if (f.advancedCandidates) {
        importExternalFormatCandidate(f, raw, objects);
        importDrmCandidate(f, raw, objects);
        createCopyBuffer(f, raw, objects);
    }
    if (f.usage == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        importTransferReference(f, raw, objects);
    if (f.usage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        importLinearCandidate(f, raw, objects);
}
}  // namespace rawrcam::diagnostics
