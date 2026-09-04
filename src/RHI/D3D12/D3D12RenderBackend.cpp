#include "RHI/D3D12/D3D12RenderBackend.h"

#include "Core/Assert.h"
#include "RHI/D3D12/D3D12Resources.h"

#include <fstream>
#include <vector>

namespace Prism::RHI::D3D12
{
D3D12RenderBackend::D3D12RenderBackend(
    const FramePacingConfiguration& framePacing)
    : m_context(framePacing),
      m_device(m_context),
      m_commandContext(m_context)
{
}

void D3D12RenderBackend::Initialize(
    Platform::Window& window)
{
    m_context.Initialize(window);
}

IFrameContext& D3D12RenderBackend::GetFrameContext()
{
    return m_context;
}

const IFrameContext&
D3D12RenderBackend::GetFrameContext() const
{
    return m_context;
}

IGraphicsDevice& D3D12RenderBackend::GetGraphicsDevice()
{
    return m_device;
}

const IGraphicsDevice&
D3D12RenderBackend::GetGraphicsDevice() const
{
    return m_device;
}

ICommandContext& D3D12RenderBackend::GetCommandContext()
{
    return m_commandContext;
}

const std::string& D3D12RenderBackend::GetAdapterName() const
{
    return m_context.GetAdapterName();
}

const GraphicsAdapterInfo&
D3D12RenderBackend::GetAdapterInfo() const
{
    return m_context.GetAdapterInfo();
}

FrameAdmissionResult D3D12RenderBackend::WaitForFrameAdmission()
{
    return m_context.WaitForFrameAdmission();
}

bool D3D12RenderBackend::ApplyFramePacingConfiguration(
    const FramePacingConfiguration& configuration,
    const std::uint64_t generation,
    std::string* outErrorMessage)
{
    return m_context.ApplyFramePacingConfiguration(
        configuration, generation, outErrorMessage);
}

const FramePacingState& D3D12RenderBackend::GetFramePacingState() const
{
    return m_context.GetFramePacingState();
}

D3D12Context& D3D12RenderBackend::GetContext()
{
    return m_context;
}

const D3D12Context& D3D12RenderBackend::GetContext() const
{
    return m_context;
}

void D3D12RenderBackend::RequestTextureCapture(
    std::filesystem::path outputPath)
{
    m_capturePath = std::move(outputPath);
    m_captureRecorded = false;
}

void D3D12RenderBackend::RecordTextureCapture(
    const ITexture* texture)
{
    if (m_capturePath.empty()
        || m_captureRecorded)
    {
        return;
    }
    const auto* d3d12Texture = dynamic_cast<
        const D3D12Texture*>(texture);
    Core::Check(
        d3d12Texture != nullptr,
        "D3D12 capture requires a D3D12 texture.");
    ID3D12Resource* sourceResource =
        d3d12Texture->GetResource();
    const D3D12_RESOURCE_DESC textureDescription =
        sourceResource->GetDesc();
    std::uint32_t rowCount = 0;
    std::uint64_t rowSize = 0;
    m_context.GetDevice()->GetCopyableFootprints(
        &textureDescription,
        0,
        1,
        0,
        &m_captureFootprint,
        &rowCount,
        &rowSize,
        &m_captureTotalBytes);

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDescription{};
    readbackDescription.Dimension =
        D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDescription.Width =
        m_captureTotalBytes;
    readbackDescription.Height = 1;
    readbackDescription.DepthOrArraySize = 1;
    readbackDescription.MipLevels = 1;
    readbackDescription.SampleDesc.Count = 1;
    readbackDescription.Layout =
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Core::ThrowIfFailed(
        m_context.GetDevice()->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDescription,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_captureReadback)),
        "Failed to create the D3D12 capture readback buffer.");

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = sourceResource;
    toCopy.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toCopy.Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_context.GetCommandList()->ResourceBarrier(
        1,
        &toCopy);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = sourceResource;
    source.Type =
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_captureReadback.Get();
    destination.Type =
        D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint =
        m_captureFootprint;
    m_context.GetCommandList()->CopyTextureRegion(
        &destination,
        0,
        0,
        0,
        &source,
        nullptr);
    std::swap(
        toCopy.Transition.StateBefore,
        toCopy.Transition.StateAfter);
    m_context.GetCommandList()->ResourceBarrier(
        1,
        &toCopy);

    m_captureWidth = static_cast<std::uint32_t>(
        textureDescription.Width);
    m_captureHeight = textureDescription.Height;
    m_captureRecorded = true;
}

bool D3D12RenderBackend::ResolveTextureCapture(
    std::string* outErrorMessage)
{
    if (!m_captureRecorded
        || m_captureReadback == nullptr)
    {
        return false;
    }
    m_context.WaitForGpu();
    void* mappedData = nullptr;
    const D3D12_RANGE readRange{
        0,
        static_cast<SIZE_T>(m_captureTotalBytes)};
    if (FAILED(m_captureReadback->Map(
            0,
            &readRange,
            &mappedData)))
    {
        if (outErrorMessage != nullptr)
        {
            *outErrorMessage =
                "Failed to map the D3D12 capture buffer.";
        }
        return false;
    }

#pragma pack(push, 1)
    struct BitmapFileHeader
    {
        std::uint16_t type = 0x4d42;
        std::uint32_t fileSize = 0;
        std::uint16_t reserved0 = 0;
        std::uint16_t reserved1 = 0;
        std::uint32_t pixelOffset = 0;
    };
    struct BitmapInfoHeader
    {
        std::uint32_t headerSize = 40;
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::uint16_t planes = 1;
        std::uint16_t bitsPerPixel = 32;
        std::uint32_t compression = 0;
        std::uint32_t imageSize = 0;
        std::int32_t xPixelsPerMeter = 2835;
        std::int32_t yPixelsPerMeter = 2835;
        std::uint32_t colorsUsed = 0;
        std::uint32_t importantColors = 0;
    };
#pragma pack(pop)

    const std::uint32_t outputRowPitch =
        m_captureWidth * 4u;
    std::vector<std::byte> bgraPixels(
        static_cast<std::size_t>(outputRowPitch)
        * m_captureHeight);
    const auto* sourceBytes =
        static_cast<const std::uint8_t*>(mappedData);
    auto* destinationBytes = reinterpret_cast<
        std::uint8_t*>(bgraPixels.data());
    for (std::uint32_t y = 0;
         y < m_captureHeight;
         ++y)
    {
        const std::uint8_t* sourceRow =
            sourceBytes
            + static_cast<std::size_t>(y)
                * m_captureFootprint.Footprint.RowPitch;
        std::uint8_t* destinationRow =
            destinationBytes
            + static_cast<std::size_t>(y)
                * outputRowPitch;
        for (std::uint32_t x = 0;
             x < m_captureWidth;
             ++x)
        {
            destinationRow[x * 4u + 0u] =
                sourceRow[x * 4u + 2u];
            destinationRow[x * 4u + 1u] =
                sourceRow[x * 4u + 1u];
            destinationRow[x * 4u + 2u] =
                sourceRow[x * 4u + 0u];
            destinationRow[x * 4u + 3u] = 255u;
        }
    }
    m_captureReadback->Unmap(0, nullptr);

    BitmapFileHeader fileHeader{};
    BitmapInfoHeader infoHeader{};
    fileHeader.pixelOffset =
        sizeof(BitmapFileHeader)
        + sizeof(BitmapInfoHeader);
    fileHeader.fileSize =
        fileHeader.pixelOffset
        + static_cast<std::uint32_t>(
            bgraPixels.size());
    infoHeader.width = static_cast<std::int32_t>(
        m_captureWidth);
    infoHeader.height = -static_cast<std::int32_t>(
        m_captureHeight);
    infoHeader.imageSize =
        static_cast<std::uint32_t>(
            bgraPixels.size());

    std::ofstream output(
        m_capturePath,
        std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (outErrorMessage != nullptr)
        {
            *outErrorMessage =
                "Failed to open the D3D12 capture path.";
        }
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(&fileHeader),
        sizeof(fileHeader));
    output.write(
        reinterpret_cast<const char*>(&infoHeader),
        sizeof(infoHeader));
    output.write(
        reinterpret_cast<const char*>(
            bgraPixels.data()),
        static_cast<std::streamsize>(
            bgraPixels.size()));

    m_captureRecorded = false;
    m_capturePath.clear();
    m_captureReadback.Reset();
    return output.good();
}
} // namespace Prism::RHI::D3D12
