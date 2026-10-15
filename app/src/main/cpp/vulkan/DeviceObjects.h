#pragma once
#include <vulkan/vulkan.h>

#include <utility>
#include <vector>
namespace rawrcam::vulkan {
// Owns images, views, buffers and memory created on one device and destroys
// them in reverse creation order. Build a partial resource in a local
// instance and append() it once complete; failure paths then just return.
class DeviceObjects final {
   public:
    explicit DeviceObjects(VkDevice device = VK_NULL_HANDLE) noexcept : device_(device) {}
    ~DeviceObjects() { reset(); }
    DeviceObjects(const DeviceObjects&) = delete;
    DeviceObjects& operator=(const DeviceObjects&) = delete;
    DeviceObjects(DeviceObjects&& other) noexcept
        : device_(std::exchange(other.device_, VK_NULL_HANDLE)), objects_(std::move(other.objects_)) {
        other.objects_.clear();
    }
    DeviceObjects& operator=(DeviceObjects&& other) noexcept {
        if (this != &other) {
            reset();
            device_ = std::exchange(other.device_, VK_NULL_HANDLE);
            objects_ = std::move(other.objects_);
            other.objects_.clear();
        }
        return *this;
    }

    VkDevice device() const noexcept { return device_; }
    // Each adopt takes ownership of a non-null handle and returns it.
    VkImage adoptImage(VkImage image) { return adopt(Kind::Image, image); }
    VkImageView adoptView(VkImageView view) { return adopt(Kind::View, view); }
    VkBuffer adoptBuffer(VkBuffer buffer) { return adopt(Kind::Buffer, buffer); }
    VkDeviceMemory adoptMemory(VkDeviceMemory memory) { return adopt(Kind::Memory, memory); }
    // Takes every object of other (same device); they are destroyed before this one's.
    void append(DeviceObjects&& other) {
        objects_.insert(objects_.end(), other.objects_.begin(), other.objects_.end());
        other.objects_.clear();
    }
    void reset() noexcept {
        for (auto it = objects_.rbegin(); it != objects_.rend(); ++it) {
            switch (it->kind) {
                case Kind::Image:
                    vkDestroyImage(device_, reinterpret_cast<VkImage>(it->handle), nullptr);
                    break;
                case Kind::View:
                    vkDestroyImageView(device_, reinterpret_cast<VkImageView>(it->handle), nullptr);
                    break;
                case Kind::Buffer:
                    vkDestroyBuffer(device_, reinterpret_cast<VkBuffer>(it->handle), nullptr);
                    break;
                case Kind::Memory:
                    vkFreeMemory(device_, reinterpret_cast<VkDeviceMemory>(it->handle), nullptr);
                    break;
            }
        }
        objects_.clear();
    }

   private:
    // Non-dispatchable handles are pointers on the 64-bit-only build.
    enum class Kind { Image, View, Buffer, Memory };
    struct Object {
        Kind kind;
        void* handle;
    };
    template <typename Handle>
    Handle adopt(Kind kind, Handle handle) {
        if (handle != VK_NULL_HANDLE) objects_.push_back({kind, reinterpret_cast<void*>(handle)});
        return handle;
    }
    VkDevice device_;
    std::vector<Object> objects_;
};
}  // namespace rawrcam::vulkan
