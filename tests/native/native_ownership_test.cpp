#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <iostream>
#include <type_traits>

#include "camera/CameraCallbackLifetime.h"
#include "imaging/FrameIngressQueue.h"
#include "imaging/FramePairer.h"
#include "vulkan/DeviceObjects.h"

#include <string>
#include <vector>
#include "support/UniqueFd.h"

struct AImage {
    int id;
};
namespace {
int imagesDeleted = 0;
}
extern "C" void AImage_delete(AImage* image) {
    if (image) {
        ++imagesDeleted;
        delete image;
    }
}
namespace {
std::vector<std::string> destroyed;
}
extern "C" {
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage h, const VkAllocationCallbacks*) {
    destroyed.push_back("image" + std::to_string(reinterpret_cast<uintptr_t>(h)));
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView h, const VkAllocationCallbacks*) {
    destroyed.push_back("view" + std::to_string(reinterpret_cast<uintptr_t>(h)));
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer h, const VkAllocationCallbacks*) {
    destroyed.push_back("buffer" + std::to_string(reinterpret_cast<uintptr_t>(h)));
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory h, const VkAllocationCallbacks*) {
    destroyed.push_back("memory" + std::to_string(reinterpret_cast<uintptr_t>(h)));
}
}
template <typename Handle>
Handle fakeHandle(uintptr_t id) {
    return reinterpret_cast<Handle>(id);
}
using namespace rawrcam;

int newFd() {
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    close(descriptors[1]);
    return descriptors[0];
}
bool closed(int fd) { return fcntl(fd, F_GETFD) == -1; }
imaging::AcquiredRawFrame frame(int id, int fd, uint64_t timestamp, uint64_t generation) {
    return {new AImage{id}, nullptr, fd, timestamp, generation};
}
int main() {
    static_assert(!std::is_copy_constructible_v<support::UniqueFd>);
    static_assert(!std::is_copy_constructible_v<imaging::RawFrameLease>);
    static_assert(std::is_nothrow_move_constructible_v<imaging::RawFrameLease>);
    int fd = newFd();
    {
        support::UniqueFd owner(fd);
        support::UniqueFd moved(std::move(owner));
        assert(owner.get() == -1 && moved.get() == fd);
        int replacement = newFd();
        moved.reset(replacement);
        assert(closed(fd));
        fd = moved.release();
        assert(!closed(fd));
    }
    close(fd);
    int leaseFd = newFd();
    {
        imaging::RawFrameLease lease(frame(1, leaseFd, 100, 10));
        imaging::RawFrameLease moved(std::move(lease));
        assert(lease.get().image == nullptr && moved.get().acquireFenceFd == leaseFd);
        auto transferred = moved.release();
        assert(!closed(leaseFd) && imagesDeleted == 0);
        AImage_delete(transferred.image);
        close(transferred.acquireFenceFd);
    }
    assert(imagesDeleted == 1 && closed(leaseFd));
    // Backlog is bounded before the consumer starts; rejected/evicted leases close exactly once.
    unsigned consumedImages = 0, consumedMetadata = 0;
    imaging::FrameIngressQueue::Drops observed{};
    imaging::FrameIngressQueue queue(
        [&](imaging::FrameIngressQueue::Event event, imaging::FrameIngressQueue::Drops drops) {
            observed.images += drops.images;
            observed.metadata += drops.metadata;
            if (std::holds_alternative<imaging::RawFrameLease>(event.payload)) ++consumedImages;
            if (std::holds_alternative<metadata::FrameMetadataSnapshot>(event.payload)) ++consumedMetadata;
        });
    int first = newFd();
    queue.enqueue(frame(2, first, 100, 10));
    int second = newFd();
    queue.enqueue(frame(3, second, 200, 10));
    assert(closed(first) && imagesDeleted == 2);
    int stale = newFd();
    queue.enqueue(frame(4, stale, 999, 9));
    assert(closed(second) && queue.latestTimestamp(10) == 200 && queue.latestTimestamp(9) == 0);
    for (int i = 0; i < 9; ++i) queue.enqueue(metadata::FrameMetadataSnapshot{});
    queue.start();
    queue.stop();
    assert(consumedImages == 1 && consumedMetadata == 8);
    assert(observed.images == 2 && observed.metadata == 1);
    assert(imagesDeleted == 4 && closed(stale));
    int rejected = newFd();
    queue.enqueue(frame(5, rejected, 300, 10));
    assert(closed(rejected) && imagesDeleted == 5);
    assert(!queue.enqueue(metadata::FrameMetadataSnapshot{}));
    // Splitting a lease: the image moves out, the fence stays and closes on replacement.
    int splitFd = newFd();
    {
        imaging::RawFrameLease lease(frame(6, splitFd, 400, 10));
        imaging::RawFrameLease image = lease.takeImage();
        assert(!lease && lease.fence() == splitFd && image && image.fence() == -1);
        int replacement = newFd();
        lease.resetFence(replacement);
        assert(closed(splitFd) && lease.fence() == replacement);
        image.reset();
        assert(imagesDeleted == 6);
    }
    // Pairer: replacement, trim, matching, and destruction release each lease once.
    {
        imaging::FramePairer pairer;
        int replaced = newFd(), oldest = newFd(), matchedFd = newFd(), pending = newFd();
        pairer.putImage(500, imaging::RawFrameLease(frame(7, replaced, 500, 10)));
        pairer.putImage(500, imaging::RawFrameLease(frame(8, oldest, 500, 10)));
        assert(closed(replaced) && imagesDeleted == 7);
        pairer.putImage(600, imaging::RawFrameLease(frame(9, matchedFd, 600, 10)));
        pairer.putImage(700, imaging::RawFrameLease(frame(10, pending, 700, 10)));
        assert(pairer.trim(2, 3) == 1 && closed(oldest) && imagesDeleted == 8);
        pairer.putMetadata(600, {});
        {
            imaging::MatchedFrame matched{};
            assert(pairer.takeMatched(600, &matched) && matched.image.fence() == matchedFd);
            assert(imagesDeleted == 8);
        }
        assert(closed(matchedFd) && imagesDeleted == 9 && !closed(pending));
    }
    assert(imagesDeleted == 10);
    // Device objects: reverse creation order; appended and moved objects are destroyed once.
    {
        const auto device = fakeHandle<VkDevice>(99);
        vulkan::DeviceObjects entry(device);
        entry.adoptImage(fakeHandle<VkImage>(1));
        entry.adoptMemory(fakeHandle<VkDeviceMemory>(2));
        entry.adoptView(VK_NULL_HANDLE);
        {
            vulkan::DeviceObjects failed(device);
            failed.adoptImage(fakeHandle<VkImage>(3));
        }
        assert((destroyed == std::vector<std::string>{"image3"}));
        vulkan::DeviceObjects candidate(device);
        candidate.adoptBuffer(fakeHandle<VkBuffer>(4));
        candidate.adoptView(fakeHandle<VkImageView>(5));
        entry.append(std::move(candidate));
        vulkan::DeviceObjects moved(std::move(entry));
        entry.reset();
        assert(destroyed.size() == 1);
    }
    assert((destroyed == std::vector<std::string>{"image3", "view5", "buffer4", "memory2", "image1"}));
    // Revocation blocks late callbacks while retaining an already admitted callback.
    struct Owner {
    } owner;
    auto lifetime = std::make_shared<camera::CameraCallbackLifetime<Owner>>();
    lifetime->owner = &owner;
    {
        camera::CameraCallbackGuard<Owner> admitted(lifetime);
        assert(admitted.owner == &owner && lifetime->inFlight == 1);
        {
            std::lock_guard<std::mutex> lock(lifetime->mutex);
            lifetime->owner = nullptr;
        }
        camera::CameraCallbackGuard<Owner> late(lifetime);
        assert(late.owner == nullptr && lifetime->inFlight == 1);
    }
    assert(lifetime->inFlight == 0);
    std::cout << "NATIVE_OWNERSHIP_PASS\n";
}
