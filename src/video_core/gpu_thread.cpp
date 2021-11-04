// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/dumping/backend.h"
#include "core/settings.h"
#include "video_core/command_processor.h"
#include "video_core/gpu_thread.h"
#include "video_core/renderer_base.h"

namespace VideoCore::GPUThread {

/// Runs the GPU thread
static void RunThread(VideoCore::RendererBase& renderer, Frontend::GraphicsContext& context,
                      SynchState& state, Core::System& system) {

    MicroProfileOnThreadCreate("GpuThread");
    Common::SetCurrentThreadName("GpuThread");

    // Wait for first GPU command before acquiring the window context
    state.WaitForCommands();

    // If emulation was stopped during disk shader loading, abort before trying to acquire context
    if (!state.is_running) {
        return;
    }

    auto current_context = context.Acquire();

    while (state.is_running) {
        CommandDataContainer next = state.queue.PopWait();

        auto command = &next.data;
        auto fence = next.fence;
        if (const auto submit_list = std::get_if<SubmitListCommand>(command)) {
            Pica::CommandProcessor::ProcessCommandList(submit_list->list, submit_list->size);
        } else if (const auto data = std::get_if<SwapBuffersCommand>(command)) {
            renderer.SwapBuffers();
            Pica::CommandProcessor::AfterSwapBuffers();
        } else if (const auto data = std::get_if<MemoryFillCommand>(command)) {
            Pica::CommandProcessor::ProcessMemoryFill(*(data->config));
            const bool is_second_filler = fence & (1llu << 63);
            Pica::CommandProcessor::AfterMemoryFill(is_second_filler);
        } else if (const auto data = std::get_if<DisplayTransferCommand>(command)) {
            Pica::CommandProcessor::ProcessDisplayTransfer(*(data->config));
            Pica::CommandProcessor::AfterDisplayTransfer();
        } else if (const auto data = std::get_if<FlushRegionCommand>(command)) {
            renderer.Rasterizer()->FlushRegion(data->addr, data->size);
        } else if (const auto data = std::get_if<FlushAndInvalidateRegionCommand>(command)) {
            renderer.Rasterizer()->FlushAndInvalidateRegion(data->addr, data->size);
        } else if (const auto data = std::get_if<InvalidateRegionCommand>(command)) {
            renderer.Rasterizer()->InvalidateRegion(data->addr, data->size);
        } else if (std::holds_alternative<EndProcessingCommand>(next.data)) {
            ASSERT(state.is_running == false);
        } else {
            UNREACHABLE();
        }
        state.signaled_fence.store(next.fence);
        if (next.block) {
            // We have to lock the write_lock to ensure that the condition_variable wait not get
            // a race between the check and the lock itself.
            std::lock_guard lk(state.write_lock);
            state.cv.notify_all();
        }
    }
}

ThreadManager::ThreadManager(Core::System& system) : system{system}, renderer{renderer} {
    synchronize_event = system.CoreTiming().RegisterEvent(
        "GPUSynchronizeEvent", [this](u64 fence, s64) { state.WaitForSynchronization(fence); });
}

ThreadManager::~ThreadManager() {
    ThreadManager::ShutDown();
}

void ThreadManager::StartThread(VideoCore::RendererBase& renderer,
                                Frontend::GraphicsContext& context) {
    thread = std::make_unique<std::thread>(RunThread, std::ref(renderer), std::ref(context),
                                           std::ref(state), std::ref(system));
    thread_id = thread->get_id();
}

void ThreadManager::ShutDown() {
    if (!state.is_running) {
        return;
    }

    {
        std::scoped_lock lk(state.write_lock);
        state.is_running = false;
        state.cv.notify_all();
    }

    if (!thread->joinable()) {
        return;
    }

    // Notify GPU thread that a shutdown is pending
    PushCommand(EndProcessingCommand(), Settings::GpuTimingMode::Skip);
    thread->join();
}

void ThreadManager::Synchronize(u64 fence, Settings::GpuTimingMode mode) {
    int timeout_us{};

    switch (mode) {
    case Settings::GpuTimingMode::Asynch:
    case Settings::GpuTimingMode::Skip:
    // PushCommand should take care of synchronous wait
    case Settings::GpuTimingMode::Synch:
        return;
    case Settings::GpuTimingMode::Asynch_10us:
        timeout_us = 10;
        break;
    case Settings::GpuTimingMode::Asynch_20us:
        timeout_us = 20;
        break;
    case Settings::GpuTimingMode::Asynch_40us:
        timeout_us = 40;
        break;
    case Settings::GpuTimingMode::Asynch_60us:
        timeout_us = 60;
        break;
    case Settings::GpuTimingMode::Asynch_80us:
        timeout_us = 80;
        break;
    case Settings::GpuTimingMode::Asynch_100us:
        timeout_us = 100;
        break;
    case Settings::GpuTimingMode::Asynch_200us:
        timeout_us = 200;
        break;
    case Settings::GpuTimingMode::Asynch_400us:
        timeout_us = 400;
        break;
    case Settings::GpuTimingMode::Asynch_600us:
        timeout_us = 600;
        break;
    case Settings::GpuTimingMode::Asynch_800us:
        timeout_us = 800;
        break;
    case Settings::GpuTimingMode::Asynch_1ms:
        timeout_us = 1000;
        break;
    case Settings::GpuTimingMode::Asynch_2ms:
        timeout_us = 2000;
        break;
    case Settings::GpuTimingMode::Asynch_4ms:
        timeout_us = 4000;
        break;
    case Settings::GpuTimingMode::Asynch_6ms:
        timeout_us = 6000;
        break;
    case Settings::GpuTimingMode::Asynch_8ms:
        timeout_us = 8000;
        break;
    }

    ASSERT(timeout_us > 0);
    system.CoreTiming().ScheduleEvent(usToCycles(timeout_us), synchronize_event, fence);
}

void ThreadManager::SubmitList(PAddr list, u32 size) {
    if (size == 0) {
        return;
    }

    PushCommand(SubmitListCommand{list, size}, Settings::values.gpu_timing_mode_submit_list);
}

void ThreadManager::SwapBuffers() {
    PushCommand(SwapBuffersCommand{}, Settings::values.gpu_timing_mode_swap_buffers);
}

void ThreadManager::DisplayTransfer(const GPU::Regs::DisplayTransferConfig* config) {
    PushCommand(DisplayTransferCommand{config}, Settings::values.gpu_timing_mode_display_transfer);
}

void ThreadManager::MemoryFill(const GPU::Regs::MemoryFillConfig* config, bool is_second_filler) {
    PushCommand(MemoryFillCommand{config, is_second_filler},
                Settings::values.gpu_timing_mode_memory_fill);
}

void ThreadManager::FlushRegion(VAddr addr, u64 size) {
    if (Settings::values.gpu_timing_mode_flush == Settings::GpuTimingMode::Skip) {
        return;
    }

    if (!IsGpuThread()) {
        PushCommand(FlushRegionCommand{addr, size}, Settings::values.gpu_timing_mode_flush);
    } else {
        renderer.Rasterizer()->FlushRegion(addr, size);
    }
}

void ThreadManager::FlushAndInvalidateRegion(VAddr addr, u64 size) {
    if (Settings::values.gpu_timing_mode_flush_and_invalidate == Settings::GpuTimingMode::Skip) {
        return;
    }

    if (!IsGpuThread()) {
        PushCommand(InvalidateRegionCommand{addr, size},
                    Settings::values.gpu_timing_mode_flush_and_invalidate);
    } else {
        renderer.Rasterizer()->InvalidateRegion(addr, size);
    }
}

void ThreadManager::InvalidateRegion(VAddr addr, u64 size) {
    if (Settings::values.gpu_timing_mode_invalidate == Settings::GpuTimingMode::Skip) {
        return;
    }

    if (!IsGpuThread()) {
        PushCommand(InvalidateRegionCommand{addr, size},
                    Settings::values.gpu_timing_mode_invalidate);
    } else {
        renderer.Rasterizer()->InvalidateRegion(addr, size);
    }
}

u64 ThreadManager::PushCommand(CommandData&& command_data, Settings::GpuTimingMode mode) {
    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
    const bool block =
        (mode != Settings::GpuTimingMode::Skip) && (mode != Settings::GpuTimingMode::Asynch);
    state.queue.Push(CommandDataContainer(std::move(command_data), fence, block));
    if (mode == Settings::GpuTimingMode::Synch) {
        state.cv.wait(lk, [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed) ||
                   !state.is_running;
        });
    } else {
        Synchronize(fence, mode);
    }
    return fence;
}

void ThreadManager::WaitForProcessing() {
    state.WaitForProcessing();
}

MICROPROFILE_DEFINE(GPU_wait, "GPU", "Wait for the GPU", MP_RGB(128, 128, 192));
void SynchState::WaitForSynchronization(u64 fence) {
    if (fence > last_fence) { // We don't want to wait infinitely
        return;
    }
    if (signaled_fence.load(std::memory_order_relaxed) >= fence) {
        return;
    }

    // Wait for the GPU to be idle (all commands to be executed)
    MICROPROFILE_SCOPE(GPU_wait);
    std::unique_lock lk(write_lock);
    cv.wait(lk, [this, fence] {
        return fence <= signaled_fence.load(std::memory_order_relaxed) || !is_running;
    });
}

} // namespace VideoCore::GPUThread
