#pragma once

#include <daxa/daxa.hpp>
#include <daxa/utils/task_graph.hpp>

#include "../cinder.hpp"

using namespace cinder;

template<typename T>
inline void allocate_fill_copy(daxa::TaskInterface ti, T value, daxa::TaskBufferAttachmentInfo dst, u32 dst_offset = 0)
{
    auto address = ti.device.get_device_address(dst.ids[0]).value();
    auto alloc = ti.allocator->allocate_fill(value).value();
    ti.recorder.copy_buffer_to_buffer({
        .src_buffer = ti.allocator->buffer(),
        .dst_buffer = dst.ids[0],
        .src_offset = alloc.buffer_offset,
        .dst_offset = dst_offset,
        .size = sizeof(T),
    });
}

void assign_blob(auto & arr, auto const & span)
{
    std::memcpy(arr.value.data(), span.data(), span.size());
}