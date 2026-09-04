#include "Scene/RenderSceneMailbox.h"

#include <stdexcept>
#include <utility>

namespace Prism::Scene
{
std::uint64_t RenderSceneMailbox::PublishFramePacket(
    std::shared_ptr<const RenderFramePacket> packet)
{
    if (packet == nullptr)
    {
        throw std::invalid_argument(
            "RenderScene mailbox cannot publish a null frame packet.");
    }
    std::scoped_lock lock(m_mutex);
    const std::uint64_t generation = ++m_generation;
    auto frame = std::make_shared<RenderSceneFrame>();
    frame->generation = generation;
    frame->packet = std::move(packet);
    m_latest = std::move(frame);
    return generation;
}

std::shared_ptr<const RenderFramePacket>
RenderSceneMailbox::AcquireLatestPacket() const
{
    std::scoped_lock lock(m_mutex);
    return m_latest != nullptr ? m_latest->packet : nullptr;
}

bool RenderSceneMailbox::HasPublishedFrame() const
{
    return AcquireLatestPacket() != nullptr;
}
} // namespace Prism::Scene
