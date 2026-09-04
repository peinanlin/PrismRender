#include "Platform/Window.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"
#include "RHI/RenderBackendFactory.h"
#include "RHI/D3D12/D3D12Resources.h"

#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void ValidateSharedSamplers(Prism::RHI::IGraphicsDevice& device, Prism::RHI::IFrameContext& frame)
{
    using namespace Prism::RHI;
    constexpr std::uint32_t SetCount = 4096u;
    constexpr std::uint32_t SamplerCount = 3u;
    DescriptorSetLayoutDescription description{};
    for (std::uint32_t index = 0; index < SamplerCount; ++index)
        description.bindings.push_back({48u + index, DescriptorType::Sampler});
    auto layout = device.CreateDescriptorSetLayout(description);
    const auto handle = [](const std::shared_ptr<IDescriptorSet>& set)
    {
        return dynamic_cast<D3D12::D3D12DescriptorSet&>(*set).GetSamplerTable().ptr;
    };
    auto sampler = device.CreateSampler({});
    const auto baseline = device.GetDescriptorAllocatorStatistics();
    for (std::uint32_t round = 0; round < 3u; ++round)
    {
        std::vector<std::shared_ptr<IDescriptorSet>> sets;
        for (std::uint32_t index = 0; index < SetCount; ++index)
        {
            auto set = device.CreateDescriptorSet(layout);
            for (std::uint32_t binding = 0; binding < SamplerCount; ++binding)
                set->WriteSampler(48u + binding, sampler);
            (void)handle(set);
            if (!sets.empty()) Expect(handle(set) == handle(sets.front()), "Identical sampler tables were not shared.");
            sets.push_back(std::move(set));
        }
        Expect(device.GetDescriptorAllocatorStatistics().allocatedSamplerDescriptorCount
                == baseline.allocatedSamplerDescriptorCount + SamplerCount,
            "4096 identical sampler tables allocated more than three physical slots.");
        const auto original = handle(sets.front());
        auto unused = device.CreateDescriptorSet(layout);
        std::vector<std::future<UINT64>> bindings;
        for (std::uint32_t worker = 0; worker < 8u; ++worker)
            bindings.push_back(std::async(std::launch::async, [&] { return handle(unused); }));
        for (auto& binding : bindings)
            Expect(binding.get() == original, "Concurrent binding of unused reflected samplers did not share valid defaults.");
        unused.reset();
        // Distinct sampler objects with equal descriptions must reuse the table.
        sets.front()->WriteSampler(48u, device.CreateSampler({}));
        Expect(handle(sets.front()) == original, "Same-value sampler update allocated a new table.");
        SamplerDescription changed{};
        changed.mipLodBias = 0.5f;
        sets.front()->WriteSampler(48u, device.CreateSampler(changed));
        Expect(handle(sets.front()) != original && handle(sets.back()) == original,
            "Changing one set overwrote a shared table.");
        Expect(device.GetDescriptorAllocatorStatistics().allocatedSamplerDescriptorCount
                == baseline.allocatedSamplerDescriptorCount + SamplerCount * 2u,
            "Changed table did not allocate one separate immutable range.");
        sets.clear();
        const auto retired = device.GetDescriptorAllocatorStatistics();
        Expect(retired.allocatedSamplerDescriptorCount == baseline.allocatedSamplerDescriptorCount + SamplerCount * 2u
                && retired.pendingReleaseCount >= baseline.pendingReleaseCount + 2u,
            "Sampler ranges were freed before their retirement fence.");
        for (std::uint32_t index = 0; index <= frame.GetFramesInFlight(); ++index)
        {
            Expect(frame.BeginFrame() == FrameResult::Ready, "Sampler BeginFrame failed.");
            Expect(frame.EndFrame() == FrameResult::Ready, "Sampler EndFrame failed.");
        }
        frame.WaitForGpu();
        Expect(device.GetDescriptorAllocatorStatistics().allocatedSamplerDescriptorCount == baseline.allocatedSamplerDescriptorCount,
            "Shared sampler ranges leaked after frame retirement.");
    }
    std::cout << "Samplers: 3 rounds of 4096 tables sharing 3 slots; copy-on-write and fence retirement passed.\n";
}
}

int main()
{
    try
    {
        using namespace Prism::RHI;
        Prism::Platform::Window window("Texture descriptor regression", 64u, 64u);
        auto backend = CreateRenderBackend(GraphicsApi::Direct3D12);
        backend->Initialize(window);
        auto& device = backend->GetGraphicsDevice();
        auto& frame = backend->GetFrameContext();
        // This fits the shader-visible heap, but the old WriteTexture path
        // allocated 16,384 additional single-slot heaps for the same bindings.
        constexpr std::uint32_t SetCount = 2048u;
        constexpr std::uint32_t BindingCount = 8u;
        constexpr std::uint32_t RoundCount = 3u;
        DescriptorSetLayoutDescription layoutDescription{};
        for (std::uint32_t binding = 0; binding < BindingCount; ++binding)
            layoutDescription.bindings.push_back({binding, DescriptorType::SampledTexture});
        auto layout = device.CreateDescriptorSetLayout(layoutDescription);
        TextureDescription textureDescription{};
        textureDescription.width = textureDescription.height = 4u;
        textureDescription.format = Format::Rgba8Unorm;
        textureDescription.usage = TextureUsage::ShaderResource;
        const auto baseline = device.GetDescriptorAllocatorStatistics();
        for (std::uint32_t round = 0; round < RoundCount; ++round)
        {
            auto texture = device.CreateTexture(textureDescription);
            std::weak_ptr<ITexture> retainedTexture = texture;
            std::vector<std::shared_ptr<IDescriptorSet>> sets;
            sets.reserve(SetCount);
            for (std::uint32_t index = 0; index < SetCount; ++index)
            {
                auto set = device.CreateDescriptorSet(layout);
                for (std::uint32_t binding = 0; binding < BindingCount; ++binding)
                    set->WriteTexture(binding, texture);
                sets.push_back(std::move(set));
            }
            texture.reset();
            Expect(!retainedTexture.expired(), "Texture binding did not retain its resource.");
            auto replacement = device.CreateTexture(textureDescription);
            for (auto& set : sets)
                for (std::uint32_t binding = 0; binding < BindingCount; ++binding)
                    set->WriteTexture(binding, replacement);
            Expect(retainedTexture.expired(), "Replacing all bindings leaked the previous texture owner.");
            const auto allocated = device.GetDescriptorAllocatorStatistics();
            Expect(allocated.allocatedResourceDescriptorCount
                    == baseline.allocatedResourceDescriptorCount + SetCount * BindingCount,
                "Texture writes allocated extra shader-visible slots.");
            retainedTexture = replacement;
            replacement.reset();
            Expect(!retainedTexture.expired(), "Replacement texture lost its binding owner.");
            sets.clear();
            Expect(retainedTexture.expired(), "Descriptor destruction leaked texture owners.");
            // Descriptor ranges retire with their frame slot; advance through all
            // slots before asserting reclamation (a GPU wait alone is insufficient).
            for (std::uint32_t index = 0; index <= frame.GetFramesInFlight(); ++index)
            {
                Expect(frame.BeginFrame() == FrameResult::Ready, "BeginFrame failed.");
                Expect(frame.EndFrame() == FrameResult::Ready, "EndFrame failed.");
            }
            frame.WaitForGpu();
            const auto reclaimed = device.GetDescriptorAllocatorStatistics();
            Expect(reclaimed.allocatedResourceDescriptorCount == baseline.allocatedResourceDescriptorCount
                    && reclaimed.pendingReleaseCount == baseline.pendingReleaseCount,
                "Descriptor ranges were not reclaimed after their frame fences.");
        }
        std::cout << "Texture descriptors: 3 rounds of 16384 bindings; replacement, retention and reclamation passed.\n";
        ValidateSharedSamplers(device, frame);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Texture descriptor regression failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
