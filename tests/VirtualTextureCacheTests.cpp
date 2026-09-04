#include "Renderer/Features/VirtualTextureCache.h"

#include "RHI/IGraphicsDevice.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class TestBuffer final : public Prism::RHI::IBuffer
{
public:
    TestBuffer(
        Prism::RHI::BufferDescription description,
        const void* const initialData)
        : m_description(std::move(description)),
          m_bytes(m_description.size)
    {
        if (initialData != nullptr)
        {
            std::memcpy(m_bytes.data(), initialData, m_bytes.size());
        }
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    const Prism::RHI::BufferDescription&
    GetDescription() const override
    {
        return m_description;
    }

    void Update(
        const void* const data,
        const std::size_t size,
        const std::size_t offset) override
    {
        Expect(offset + size <= m_bytes.size(),
            "Virtual texture test buffer update exceeded its allocation.");
        std::memcpy(m_bytes.data() + offset, data, size);
    }

    void Read(
        void* const data,
        const std::size_t size,
        const std::size_t offset) const override
    {
        Expect(offset + size <= m_bytes.size(),
            "Virtual texture test buffer read exceeded its allocation.");
        std::memcpy(data, m_bytes.data() + offset, size);
    }

private:
    Prism::RHI::BufferDescription m_description;
    std::vector<std::uint8_t> m_bytes;
};

class TestTexture final : public Prism::RHI::ITexture
{
public:
    explicit TestTexture(Prism::RHI::TextureDescription description)
        : m_description(std::move(description))
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    const Prism::RHI::TextureDescription&
    GetDescription() const override
    {
        return m_description;
    }

private:
    Prism::RHI::TextureDescription m_description;
};

class TestTextureView final : public Prism::RHI::ITextureView
{
public:
    TestTextureView(
        std::shared_ptr<Prism::RHI::ITexture> texture,
        Prism::RHI::TextureViewDescription description)
        : m_texture(std::move(texture)),
          m_description(std::move(description))
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    const Prism::RHI::TextureViewDescription&
    GetDescription() const override
    {
        return m_description;
    }

    const Prism::RHI::ITexture* GetTexture() const override
    {
        return m_texture.get();
    }

private:
    std::shared_ptr<Prism::RHI::ITexture> m_texture;
    Prism::RHI::TextureViewDescription m_description;
};

class TestGraphicsDevice final : public Prism::RHI::IGraphicsDevice
{
public:
    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    Prism::RHI::ShaderBinaryFormat
    GetPreferredShaderBinaryFormat() const override
    {
        return Prism::RHI::ShaderBinaryFormat::SpirV;
    }

    const Prism::RHI::GraphicsDeviceCapabilities&
    GetCapabilities() const override
    {
        return m_capabilities;
    }

    Prism::RHI::DescriptorAllocatorStatistics
    GetDescriptorAllocatorStatistics() const override
    {
        return {};
    }

    Prism::RHI::UploadQueueStatistics
    GetUploadQueueStatistics() const override
    {
        return {};
    }

    Prism::RHI::UploadTicket GetPendingUploadTicket() const override
    {
        return {};
    }

    bool IsUploadComplete(Prism::RHI::UploadTicket) const override
    {
        return true;
    }

    Prism::RHI::ResourceRetirementStatistics
    GetResourceRetirementStatistics() const override
    {
        return {};
    }

    std::shared_ptr<Prism::RHI::IBuffer> CreateBuffer(
        const Prism::RHI::BufferDescription& description,
        const void* const initialData) override
    {
        return std::make_shared<TestBuffer>(description, initialData);
    }

    std::shared_ptr<Prism::RHI::ITexture> CreateTexture(
        const Prism::RHI::TextureDescription& description,
        const Prism::RHI::TextureInitialData*) override
    {
        ++createdTextureCount;
        return std::make_shared<TestTexture>(description);
    }

    std::shared_ptr<Prism::RHI::ITransientTexturePool>
    CreateTransientTexturePool(
        const std::vector<Prism::RHI::TransientTextureRequest>&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::ITransientBufferPool>
    CreateTransientBufferPool(
        const std::vector<Prism::RHI::TransientBufferRequest>&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::ITextureView> CreateTextureView(
        std::shared_ptr<Prism::RHI::ITexture> texture,
        const Prism::RHI::TextureViewDescription& description) override
    {
        return std::make_shared<TestTextureView>(
            std::move(texture), description);
    }

    std::shared_ptr<Prism::RHI::ISampler> CreateSampler(
        const Prism::RHI::SamplerDescription&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IDescriptorSetLayout>
    CreateDescriptorSetLayout(
        const Prism::RHI::DescriptorSetLayoutDescription&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IDescriptorSet> CreateDescriptorSet(
        std::shared_ptr<Prism::RHI::IDescriptorSetLayout>) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IGraphicsPipeline>
    CreateGraphicsPipeline(
        const Prism::RHI::GraphicsPipelineDescription&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IComputePipeline>
    CreateComputePipeline(
        const Prism::RHI::ComputePipelineDescription&) override
    {
        return nullptr;
    }

    Prism::RHI::AccelerationStructureBuildSizes
    QueryAccelerationStructureBuildSizes(
        const Prism::RHI::AccelerationStructureBuildDescription&) const override
    {
        return {};
    }

    std::shared_ptr<Prism::RHI::IRayTracingAccelerationStructure>
    CreateAccelerationStructure(
        const Prism::RHI::AccelerationStructureBuildRequest&) override
    {
        return nullptr;
    }

    std::size_t createdTextureCount = 0;

private:
    Prism::RHI::GraphicsDeviceCapabilities m_capabilities{};
};

void TestResidencyResetAndFrameSlotRetirement()
{
    constexpr std::uint32_t FramesInFlight = 2;
    TestGraphicsDevice device;
    Prism::Renderer::VirtualTextureCache cache;
    cache.Initialize(device, FramesInFlight);

    Expect(cache.IsInitialized(),
        "Virtual texture cache did not initialize.");
    Expect(device.createdTextureCount == 1u,
        "Virtual texture cache did not publish its initial atlas.");
    Expect(cache.GetStatistics().residentPages == 1u,
        "Virtual texture cache initial residency changed.");

    std::weak_ptr<Prism::RHI::ITexture> initialAtlas =
        cache.GetAtlasTexture();
    Expect(cache.Update(0u, {0.0f, 0.0f, 0.0f}, 4096.0f, true),
        "First virtual texture request did not populate residency.");
    Expect(cache.GetStatistics().requestedPages == 49u,
        "Virtual texture request radius changed.");
    Expect(cache.GetStatistics().pageMisses > 0u,
        "First virtual texture request unexpectedly had no misses.");
    Expect(cache.RetiredAtlasCount() == 1u && !initialAtlas.expired(),
        "Replaced atlas was not retained by its in-flight frame slot.");

    std::weak_ptr<Prism::RHI::ITexture> centerAtlas =
        cache.GetAtlasTexture();
    Expect(cache.Update(1u, {2047.0f, 0.0f, 2047.0f}, 4096.0f, true),
        "Camera movement did not update virtual texture residency.");
    Expect(cache.RetiredAtlasCount() == FramesInFlight,
        "Atlas retirement exceeded or failed to fill the frame ring.");
    Expect(!centerAtlas.expired(),
        "Current atlas was released before the consuming frame slot retired.");

    cache.Update(0u, {0.0f, 0.0f, 0.0f}, 4096.0f, true);
    Expect(initialAtlas.expired(),
        "Atlas survived after its frame slot fence was recycled.");
    Expect(cache.RetiredAtlasCount() <= FramesInFlight,
        "Atlas retirement grew beyond the frames-in-flight bound.");

    const std::uint64_t generationBeforeReset =
        cache.GetStatistics().generation;
    cache.RequestReset();
    Expect(cache.IsResetPending(),
        "Virtual texture scene reset was not deferred.");
    Expect(cache.Update(1u, {0.0f, 0.0f, 0.0f}, 4096.0f, false),
        "Virtual texture scene reset did not rebuild the atlas.");
    Expect(!cache.IsResetPending(),
        "Virtual texture scene reset remained pending after update.");
    Expect(cache.GetStatistics().residentPages == 1u,
        "Virtual texture scene reset did not restore initial residency.");
    Expect(cache.GetStatistics().generation == generationBeforeReset + 1u,
        "Virtual texture scene reset did not publish a new generation.");
    Expect(cache.RetiredAtlasCount() <= FramesInFlight,
        "Reset atlas retirement exceeded the frame ring.");
}
} // namespace

int main()
{
    try
    {
        TestResidencyResetAndFrameSlotRetirement();
        std::cout << "Virtual texture cache tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Virtual texture cache tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
