#pragma once

#include "Scene/RenderFramePacket.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace Prism::Scene
{
// Immutable publication boundary between scene mutation and render
// consumption. The current host publishes and acquires on one thread; a
// dedicated render thread can consume the same mailbox without exposing the
// mutable editor/world scene.
struct RenderSceneFrame
{
    std::uint64_t generation = 0;
    std::shared_ptr<const RenderFramePacket> packet;
};

class RenderSceneMailbox
{
public:
    [[nodiscard]] std::uint64_t PublishFramePacket(
        std::shared_ptr<const RenderFramePacket> packet);
    [[nodiscard]] std::shared_ptr<const RenderFramePacket>
        AcquireLatestPacket() const;
    [[nodiscard]] bool HasPublishedFrame() const;

private:
    // MSVC implements atomic<shared_ptr> through the optional
    // msvcp140_atomic_wait runtime. That DLL is not guaranteed to be
    // deployable beside PrismRender and caused optimized builds to fail in
    // the Windows loader before main(). Publication is low-frequency, so a
    // small mutex keeps the same thread-safe snapshot contract without an
    // extra runtime dependency.
    mutable std::mutex m_mutex;
    std::uint64_t m_generation = 0;
    std::shared_ptr<const RenderSceneFrame> m_latest;
};
} // namespace Prism::Scene
