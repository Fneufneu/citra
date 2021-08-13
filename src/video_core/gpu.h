// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "core/hw/gpu.h"
#include "video_core/gpu_thread.h"

namespace Core {
class System;
}

namespace Frontend {
class GraphicsContext;
}

namespace VideoCore {

class RendererBase;

class GPUBackend {
public:
    explicit GPUBackend(std::unique_ptr<VideoCore::RendererBase>&& renderer);

    virtual ~GPUBackend();

    virtual void Start() = 0;
    virtual void ProcessCommandList(PAddr list, u32 size) = 0;
    virtual void SwapBuffers() = 0;
    virtual void DisplayTransfer(const GPU::Regs::DisplayTransferConfig* config) = 0;
    virtual void MemoryFill(const GPU::Regs::MemoryFillConfig* config, bool is_second_filler) = 0;
    virtual void FlushRegion(VAddr addr, u64 size) = 0;
    virtual void FlushAndInvalidateRegion(VAddr addr, u64 size) = 0;
    virtual void InvalidateRegion(VAddr addr, u64 size) = 0;
    virtual void WaitForProcessing();

    VideoCore::RendererBase& Renderer() {
        return *renderer;
    }

    const VideoCore::RendererBase& Renderer() const {
        return *renderer;
    }

protected:
    std::unique_ptr<VideoCore::RendererBase> renderer;
};

class GPUSerial : public GPUBackend {
public:
    explicit GPUSerial(Core::System& system, std::unique_ptr<VideoCore::RendererBase>&& renderer,
                       std::unique_ptr<Frontend::GraphicsContext>&& context);

    ~GPUSerial();

    void Start() override;
    void ProcessCommandList(PAddr list, u32 size) override;
    void SwapBuffers() override;
    void DisplayTransfer(const GPU::Regs::DisplayTransferConfig* config) override;
    void MemoryFill(const GPU::Regs::MemoryFillConfig* config, bool is_second_filler) override;
    void FlushRegion(VAddr addr, u64 size) override;
    void FlushAndInvalidateRegion(VAddr addr, u64 size) override;
    void InvalidateRegion(VAddr addr, u64 size) override;

private:
    Core::System& system;
    std::unique_ptr<Frontend::GraphicsContext> context;
};

class GPUParallel : public GPUBackend {
public:
    explicit GPUParallel(Core::System& system, std::unique_ptr<VideoCore::RendererBase>&& renderer,
                         std::unique_ptr<Frontend::GraphicsContext>&& context);

    ~GPUParallel();

    void Start() override;
    void ProcessCommandList(PAddr list, u32 size) override;
    void SwapBuffers() override;
    void DisplayTransfer(const GPU::Regs::DisplayTransferConfig* config) override;
    void MemoryFill(const GPU::Regs::MemoryFillConfig* config, bool is_second_filler) override;
    void FlushRegion(VAddr addr, u64 size) override;
    void FlushAndInvalidateRegion(VAddr addr, u64 size) override;
    void InvalidateRegion(VAddr addr, u64 size) override;
    void WaitForProcessing() override;

private:
    GPUThread::ThreadManager gpu_thread;
    std::unique_ptr<Frontend::GraphicsContext> cpu_context;
    std::unique_ptr<Frontend::GraphicsContext> gpu_context;
};

} // namespace VideoCore
