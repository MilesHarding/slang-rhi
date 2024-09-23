#include "wgpu-device.h"
#include "wgpu-buffer.h"
#include "wgpu-shader-object.h"
#include "wgpu-shader-object-layout.h"

#include "core/common.h"
#include "core/deferred.h"

#include <cstdio>
#include <vector>

namespace rhi::wgpu {

static void errorCallback(WGPUErrorType type, char const* message, void* userdata)
{
    DeviceImpl* device = static_cast<DeviceImpl*>(userdata);
    device->handleError(type, message);
}

Context::~Context()
{
    if (device)
    {
        api.wgpuDeviceRelease(device);
    }
    if (adapter)
    {
        api.wgpuAdapterRelease(adapter);
    }
    if (instance)
    {
        api.wgpuInstanceRelease(instance);
    }
}

DeviceImpl::~DeviceImpl() {}

Result DeviceImpl::getNativeDeviceHandles(NativeHandles* outHandles)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

void DeviceImpl::handleError(WGPUErrorType type, char const* message)
{
    fprintf(stderr, "WGPU error: %s\n", message);
}

Result DeviceImpl::initialize(const Desc& desc)
{
    SLANG_RETURN_ON_FAIL(m_ctx.api.init());
    API& api = m_ctx.api;

    // Initialize device info.
    {
        m_info.apiName = "WGPU";
        m_info.deviceType = DeviceType::WGPU;
        m_info.adapterName = "default";
        static const float kIdentity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        ::memcpy(m_info.identityProjectionMatrix, kIdentity, sizeof(kIdentity));
    }

    m_desc = desc;

    SLANG_RETURN_ON_FAIL(Device::initialize(desc));
    Result initDeviceResult = SLANG_OK;
    SLANG_RETURN_ON_FAIL(slangContext.initialize(
        desc.slang,
        desc.extendedDescCount,
        desc.extendedDescs,
        SLANG_WGSL,
        "",
        std::array{slang::PreprocessorMacroDesc{"__WGPU__", "1"}}
    ));

    WGPUInstanceDescriptor instanceDesc = {};
    m_ctx.instance = api.wgpuCreateInstance(&instanceDesc);

    auto requestAdapterCallback =
        [](WGPURequestAdapterStatus status, WGPUAdapter adapter, char const* message, WGPU_NULLABLE void* userdata)
    {
        Context* ctx = (Context*)userdata;
        if (status == WGPURequestAdapterStatus_Success)
        {
            ctx->adapter = adapter;
        }
    };

    api.wgpuInstanceRequestAdapter(m_ctx.instance, nullptr, requestAdapterCallback, &m_ctx);
    if (!m_ctx.adapter)
    {
        return SLANG_FAIL;
    }

    auto requestDeviceCallback =
        [](WGPURequestDeviceStatus status, WGPUDevice device, char const* message, void* userdata)
    {
        Context* ctx = (Context*)userdata;
        if (status == WGPURequestDeviceStatus_Success)
        {
            ctx->device = device;
        }
    };

    WGPUDeviceDescriptor deviceDesc = {};
    deviceDesc.uncapturedErrorCallbackInfo.callback = errorCallback;
    deviceDesc.uncapturedErrorCallbackInfo.userdata = this;
    api.wgpuAdapterRequestDevice(m_ctx.adapter, &deviceDesc, requestDeviceCallback, &m_ctx);
    if (!m_ctx.device)
    {
        return SLANG_FAIL;
    }

    WGPUSupportedLimits limits = {};
    api.wgpuDeviceGetLimits(m_ctx.device, &limits);

    m_info.limits.maxComputeDispatchThreadGroups[0] = limits.limits.maxComputeWorkgroupSizeX;

    return SLANG_OK;
}

const DeviceInfo& DeviceImpl::getDeviceInfo() const
{
    return m_info;
}

Result DeviceImpl::createSwapchain(const ISwapchain::Desc& desc, WindowHandle window, ISwapchain** outSwapchain)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::readTexture(
    ITexture* texture,
    ResourceState state,
    ISlangBlob** outBlob,
    Size* outRowPitch,
    Size* outPixelSize
)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::readBuffer(IBuffer* buffer, Offset offset, Size size, ISlangBlob** outBlob)
{
    BufferImpl* bufferImpl = static_cast<BufferImpl*>(buffer);

    WGPUBufferDescriptor stagingBufferDesc = {};
    stagingBufferDesc.size = size;
    stagingBufferDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer stagingBuffer = m_ctx.api.wgpuDeviceCreateBuffer(m_ctx.device, &stagingBufferDesc);
    if (!stagingBuffer)
    {
        return SLANG_FAIL;
    }
    SLANG_RHI_DEFERRED({ m_ctx.api.wgpuBufferRelease(stagingBuffer); });

    WGPUCommandEncoder encoder = m_ctx.api.wgpuDeviceCreateCommandEncoder(m_ctx.device, nullptr);
    if (!encoder)
    {
        return SLANG_FAIL;
    }
    SLANG_RHI_DEFERRED({ m_ctx.api.wgpuCommandEncoderRelease(encoder); });

    m_ctx.api.wgpuCommandEncoderCopyBufferToBuffer(encoder, bufferImpl->m_buffer, offset, stagingBuffer, 0, size);
    WGPUCommandBuffer commandBuffer = m_ctx.api.wgpuCommandEncoderFinish(encoder, nullptr);
    if (!commandBuffer)
    {
        return SLANG_FAIL;
    }
    SLANG_RHI_DEFERRED({ m_ctx.api.wgpuCommandBufferRelease(commandBuffer); });

    WGPUQueue queue = m_ctx.api.wgpuDeviceGetQueue(m_ctx.device);
    m_ctx.api.wgpuQueueSubmit(queue, 1, &commandBuffer);

    // Wait for the command buffer to finish executing
    // TODO: we should switch to the new async API
    {
        WGPUQueueWorkDoneStatus status = WGPUQueueWorkDoneStatus_Unknown;
        m_ctx.api.wgpuQueueOnSubmittedWorkDone(
            queue,
            [](WGPUQueueWorkDoneStatus status, void* userdata) { *(WGPUQueueWorkDoneStatus*)userdata = status; },
            &status
        );
        while (status == WGPUQueueWorkDoneStatus_Unknown)
        {
            m_ctx.api.wgpuDeviceTick(m_ctx.device);
        }
        if (status != WGPUQueueWorkDoneStatus_Success)
        {
            return SLANG_FAIL;
        }
    }

    // Map the staging buffer
    // TODO: we should switch to the new async API
    {
        WGPUBufferMapAsyncStatus status = WGPUBufferMapAsyncStatus_Unknown;
        m_ctx.api.wgpuBufferMapAsync(
            stagingBuffer,
            WGPUMapMode_Read,
            0,
            size,
            [](WGPUBufferMapAsyncStatus status, void* userdata) { *(WGPUBufferMapAsyncStatus*)userdata = status; },
            &status
        );
        while (status == WGPUBufferMapAsyncStatus_Unknown)
        {
            m_ctx.api.wgpuDeviceTick(m_ctx.device);
        }
        if (status != WGPUBufferMapAsyncStatus_Success)
        {
            return SLANG_FAIL;
        }
    }
    SLANG_RHI_DEFERRED({ m_ctx.api.wgpuBufferUnmap(stagingBuffer); });

#if 0
    WGPUQueueWorkDoneStatus workDoneStatus = WGPUQueueWorkDoneStatus_Unknown;
    WGPUQueueWorkDoneCallbackInfo2 workDoneCallbackInfo = {};
    workDoneCallbackInfo.mode = WGPUCallbackMode_WaitAnyOnly;
    workDoneCallbackInfo.callback = [](WGPUQueueWorkDoneStatus status, void* userdata1, void* userdata2)
    { *(WGPUQueueWorkDoneStatus*)userdata1 = status; };
    workDoneCallbackInfo.userdata1 = &workDoneStatus;
    WGPUFuture workDoneFuture = m_ctx.api.wgpuQueueOnSubmittedWorkDone2(queue, workDoneCallbackInfo);
    WGPUFutureWaitInfo workDoneWaitInfo = {};
    workDoneWaitInfo.future = workDoneFuture;
    m_ctx.api.wgpuInstanceWaitAny(m_ctx.instance, 1, &workDoneWaitInfo, uint64_t(-1));
#endif

#if 0
    WGPUMapAsyncStatus mapStatus = WGPUMapAsyncStatus_Unknown;
    WGPUBufferMapCallbackInfo2 callbackInfo = {};
    callbackInfo.callback = [](WGPUMapAsyncStatus status, char const* message, void* userdata1, void* userdata2)
    { *(WGPUMapAsyncStatus*)userdata1 = status; };
    callbackInfo.mode = WGPUCallbackMode_WaitAnyOnly;
    callbackInfo.userdata1 = &mapStatus;
    WGPUFuture future = m_ctx.api.wgpuBufferMapAsync2(stagingBuffer, WGPUMapMode_Read, 0, size, callbackInfo);
    WGPUFutureWaitInfo waitInfo = {};
    waitInfo.future = future;
    m_ctx.api.wgpuInstanceWaitAny(m_ctx.instance, 1, &waitInfo, uint64_t(-1));
    m_ctx.api.wgpuInstanceWaitAny(m_ctx.instance, 1, &waitInfo, uint64_t(-1));
    if (!waitInfo.completed || mapStatus != WGPUMapAsyncStatus_Success)
    {
        return SLANG_FAIL;
    }
#endif

    const void* data = m_ctx.api.wgpuBufferGetConstMappedRange(stagingBuffer, 0, size);
    if (!data)
    {
        return SLANG_FAIL;
    }

    auto blob = OwnedBlob::create(size);
    ::memcpy((void*)blob->getBufferPointer(), data, size);

    returnComPtr(outBlob, blob);
    return SLANG_OK;
}

Result DeviceImpl::getAccelerationStructurePrebuildInfo(
    const IAccelerationStructure::BuildInputs& buildInputs,
    IAccelerationStructure::PrebuildInfo* outPrebuildInfo
)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::createAccelerationStructure(
    const IAccelerationStructure::CreateDesc& desc,
    IAccelerationStructure** outAS
)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::getTextureAllocationInfo(const TextureDesc& descIn, Size* outSize, Size* outAlignment)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::getTextureRowAlignment(Size* outAlignment)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::getFormatSupport(Format format, FormatSupport* outFormatSupport)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::createShaderObjectLayout(
    slang::ISession* session,
    slang::TypeLayoutReflection* typeLayout,
    ShaderObjectLayout** outLayout
)
{
    RefPtr<ShaderObjectLayoutImpl> layout;
    SLANG_RETURN_ON_FAIL(ShaderObjectLayoutImpl::createForElementType(this, session, typeLayout, layout.writeRef()));
    returnRefPtrMove(outLayout, layout);
    return SLANG_OK;
}

Result DeviceImpl::createShaderObject(ShaderObjectLayout* layout, IShaderObject** outObject)
{
    RefPtr<ShaderObjectImpl> shaderObject;
    SLANG_RETURN_ON_FAIL(
        ShaderObjectImpl::create(this, static_cast<ShaderObjectLayoutImpl*>(layout), shaderObject.writeRef())
    );
    returnComPtr(outObject, shaderObject);
    return SLANG_OK;
}

Result DeviceImpl::createMutableShaderObject(ShaderObjectLayout* layout, IShaderObject** outObject)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::createMutableRootShaderObject(IShaderProgram* program, IShaderObject** outObject)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

Result DeviceImpl::createShaderTable(const IShaderTable::Desc& desc, IShaderTable** outShaderTable)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

} // namespace rhi::wgpu