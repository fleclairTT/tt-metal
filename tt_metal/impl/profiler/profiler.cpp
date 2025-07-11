// SPDX-FileCopyrightText: © 2023 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "dev_msgs.h"
#include <device.hpp>
#include <distributed.hpp>
#include "device_pool.hpp"
#include "tools/profiler/event_metadata.hpp"
#include "distributed/fd_mesh_command_queue.hpp"
#include <host_api.hpp>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <tracy/TracyTTDevice.hpp>
#include <tt_metal.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "assert.hpp"
#include "dispatch/hardware_command_queue.hpp"
#include "dispatch/kernels/cq_commands.hpp"
#include "hal_types.hpp"
#include "hostdevcommon/profiler_common.h"
#include "llrt.hpp"
#include <tt-logger/tt-logger.hpp>
#include "metal_soc_descriptor.h"
#include "profiler.hpp"
#include "profiler_paths.hpp"
#include "profiler_state.hpp"
#include "tools/profiler/noc_event_profiler_utils.hpp"
#include "tracy/Tracy.hpp"
#include "tt_backend_api_types.hpp"
#include "impl/context/metal_context.hpp"
#include <umd/device/tt_core_coordinates.h>
#include <umd/device/types/arch.h>
#include <umd/device/types/xy_pair.h>
#include <tt-metalium/device_pool.hpp>
#include "fabric_edm_packet_header.hpp"
#include "fabric/fabric_host_utils.hpp"
#include "tt_cluster.hpp"

namespace tt {

namespace tt_metal {

static kernel_profiler::PacketTypes get_packet_type(uint32_t timer_id) {
    return static_cast<kernel_profiler::PacketTypes>((timer_id >> 16) & 0x7);
}

std::vector<uint32_t> read_control_buffer_from_core(
    IDevice* device, const CoreCoord& core, const HalProgrammableCoreType core_type, const ProfilerDumpState state) {
    std::vector<uint32_t> control_buffer;
    profiler_msg_t* profiler_msg =
        MetalContext::instance().hal().get_dev_addr<profiler_msg_t*>(core_type, HalL1MemAddrType::PROFILER);
    if (state != ProfilerDumpState::FORCE_UMD_READ && tt::DevicePool::instance().is_dispatch_firmware_active()) {
        if (auto mesh_device = device->get_mesh_device()) {
            distributed::FDMeshCommandQueue& mesh_cq =
                dynamic_cast<distributed::FDMeshCommandQueue&>(mesh_device->mesh_command_queue());
            const distributed::MeshCoordinate device_coord = mesh_device->get_view().find_device(device->id());
            const distributed::DeviceMemoryAddress address = {
                device_coord, core, reinterpret_cast<DeviceAddr>(profiler_msg->control_vector)};
            control_buffer.resize(kernel_profiler::PROFILER_L1_CONTROL_VECTOR_SIZE);
            mesh_cq.enqueue_read_shard_from_core(
                address, control_buffer.data(), kernel_profiler::PROFILER_L1_CONTROL_BUFFER_SIZE, true);
        } else {
            control_buffer.resize(kernel_profiler::PROFILER_L1_CONTROL_VECTOR_SIZE);
            dynamic_cast<HWCommandQueue&>(device->command_queue())
                .enqueue_read_from_core(
                    core,
                    control_buffer.data(),
                    reinterpret_cast<DeviceAddr>(profiler_msg->control_vector),
                    kernel_profiler::PROFILER_L1_CONTROL_BUFFER_SIZE,
                    true);
        }
    } else {
        control_buffer = tt::llrt::read_hex_vec_from_core(
            device->id(),
            core,
            reinterpret_cast<uint64_t>(profiler_msg->control_vector),
            kernel_profiler::PROFILER_L1_CONTROL_BUFFER_SIZE);
    }

    return control_buffer;
}

void write_control_buffer_to_core(
    IDevice* device,
    const CoreCoord& core,
    const HalProgrammableCoreType core_type,
    const ProfilerDumpState state,
    const std::vector<uint32_t>& control_buffer) {
    profiler_msg_t* profiler_msg =
        MetalContext::instance().hal().get_dev_addr<profiler_msg_t*>(core_type, HalL1MemAddrType::PROFILER);
    if (state != ProfilerDumpState::FORCE_UMD_READ && tt::DevicePool::instance().is_dispatch_firmware_active()) {
        if (auto mesh_device = device->get_mesh_device()) {
            distributed::FDMeshCommandQueue& mesh_cq =
                dynamic_cast<distributed::FDMeshCommandQueue&>(mesh_device->mesh_command_queue());
            const distributed::MeshCoordinate device_coord = mesh_device->get_view().find_device(device->id());
            const distributed::DeviceMemoryAddress address = {
                device_coord, core, reinterpret_cast<DeviceAddr>(profiler_msg->control_vector)};
            mesh_cq.enqueue_write_shard_to_core(
                address, control_buffer.data(), kernel_profiler::PROFILER_L1_CONTROL_BUFFER_SIZE, true);
        } else {
            dynamic_cast<HWCommandQueue&>(device->command_queue())
                .enqueue_write_to_core(
                    core,
                    control_buffer.data(),
                    reinterpret_cast<DeviceAddr>(profiler_msg->control_vector),
                    kernel_profiler::PROFILER_L1_CONTROL_BUFFER_SIZE,
                    true);
        }
    } else {
        tt::llrt::write_hex_vec_to_core(
            device->id(), core, control_buffer, reinterpret_cast<uint64_t>(profiler_msg->control_vector));
    }
}

bool useSlowDispatchForReading(ProfilerDumpState state) {
    return state == ProfilerDumpState::FORCE_UMD_READ || onlyProfileDispatchCores(state);
}

void DeviceProfiler::issueFastDispatchReadFromProfilerBuffer(IDevice* device) {
    ZoneScoped;
    TT_ASSERT(tt::DevicePool::instance().is_dispatch_firmware_active());
    const DeviceAddr profiler_addr = MetalContext::instance().hal().get_dev_addr(HalDramMemAddrType::PROFILER);
    uint32_t profile_buffer_idx = 0;

    const CoreCoord dram_grid_size = device->dram_grid_size();
    for (uint32_t x = 0; x < dram_grid_size.x; ++x) {
        for (uint32_t y = 0; y < dram_grid_size.y; ++y) {
            const CoreCoord dram_core = device->virtual_core_from_logical_core({x, y}, CoreType::DRAM);
            if (auto mesh_device = device->get_mesh_device()) {
                const distributed::MeshCoordinate device_coord = mesh_device->get_view().find_device(device->id());
                dynamic_cast<distributed::FDMeshCommandQueue&>(mesh_device->mesh_command_queue())
                    .enqueue_read_shard_from_core(
                        distributed::DeviceMemoryAddress{device_coord, dram_core, profiler_addr},
                        &(profile_buffer[profile_buffer_idx]),
                        profile_buffer_bank_size_bytes,
                        true);
            } else {
                dynamic_cast<HWCommandQueue&>(device->command_queue())
                    .enqueue_read_from_core(
                        dram_core,
                        &(profile_buffer[profile_buffer_idx]),
                        profiler_addr,
                        profile_buffer_bank_size_bytes,
                        true);
            }
            profile_buffer_idx += profile_buffer_bank_size_bytes / sizeof(uint32_t);
        }
    }
}

void DeviceProfiler::issueSlowDispatchReadFromProfilerBuffer(IDevice* device) {
    ZoneScoped;
    const DeviceAddr profiler_addr = MetalContext::instance().hal().get_dev_addr(HalDramMemAddrType::PROFILER);
    uint32_t profile_buffer_idx = 0;
    const int num_dram_channels = device->num_dram_channels();
    for (int dram_channel = 0; dram_channel < num_dram_channels; ++dram_channel) {
        std::vector<uint32_t> profile_buffer_bank_data(profile_buffer_bank_size_bytes / sizeof(uint32_t), 0);
        tt::tt_metal::MetalContext::instance().get_cluster().read_dram_vec(
            profile_buffer_bank_data.data(), profile_buffer_bank_size_bytes, device->id(), dram_channel, profiler_addr);

        std::copy(
            profile_buffer_bank_data.begin(),
            profile_buffer_bank_data.end(),
            profile_buffer.begin() + profile_buffer_idx);
        profile_buffer_idx += profile_buffer_bank_size_bytes / sizeof(uint32_t);
    }
}

std::vector<uint32_t> DeviceProfiler::issueFastDispatchReadFromL1DataBuffer(
    IDevice* device, const CoreCoord& worker_core) {
    ZoneScoped;

    TT_ASSERT(tt::DevicePool::instance().is_dispatch_firmware_active());

    const chip_id_t device_id = device->id();
    const Hal& hal = MetalContext::instance().hal();
    const HalProgrammableCoreType core_type = tt::llrt::get_core_type(device_id, worker_core);
    profiler_msg_t* profiler_msg = hal.get_dev_addr<profiler_msg_t*>(core_type, HalL1MemAddrType::PROFILER);
    const uint32_t num_risc_processors = hal.get_num_risc_processors(core_type);
    std::vector<uint32_t> data_buffer(kernel_profiler::PROFILER_L1_VECTOR_SIZE * num_risc_processors);
    if (auto mesh_device = device->get_mesh_device()) {
        const distributed::MeshCoordinate device_coord = mesh_device->get_view().find_device(device_id);
        dynamic_cast<distributed::FDMeshCommandQueue&>(mesh_device->mesh_command_queue())
            .enqueue_read_shard_from_core(
                distributed::DeviceMemoryAddress{
                    device_coord, worker_core, reinterpret_cast<DeviceAddr>(profiler_msg->buffer)},
                data_buffer.data(),
                kernel_profiler::PROFILER_L1_BUFFER_SIZE * num_risc_processors,
                true);
    } else {
        dynamic_cast<HWCommandQueue&>(device->command_queue())
            .enqueue_read_from_core(
                worker_core,
                data_buffer.data(),
                reinterpret_cast<DeviceAddr>(profiler_msg->buffer),
                kernel_profiler::PROFILER_L1_BUFFER_SIZE * num_risc_processors,
                true);
    }

    return data_buffer;
}

std::vector<uint32_t> DeviceProfiler::issueSlowDispatchReadFromL1DataBuffer(
    IDevice* device, const CoreCoord& worker_core) {
    ZoneScoped;

    const chip_id_t device_id = device->id();
    const Hal& hal = MetalContext::instance().hal();
    const HalProgrammableCoreType core_type = tt::llrt::get_core_type(device_id, worker_core);
    profiler_msg_t* profiler_msg = hal.get_dev_addr<profiler_msg_t*>(core_type, HalL1MemAddrType::PROFILER);
    return tt::llrt::read_hex_vec_from_core(
        device_id,
        worker_core,
        reinterpret_cast<uint64_t>(profiler_msg->buffer),
        kernel_profiler::PROFILER_L1_BUFFER_SIZE * hal.get_num_risc_processors(core_type));
}

void DeviceProfiler::readControlBuffers(IDevice* device, const CoreCoord& worker_core, const ProfilerDumpState state) {
    ZoneScoped;
    chip_id_t device_id = device->id();

    HalProgrammableCoreType CoreType = tt::llrt::get_core_type(device_id, worker_core);

    std::vector<uint32_t> control_buffer = read_control_buffer_from_core(device, worker_core, CoreType, state);
    core_control_buffers[worker_core] = control_buffer;
}

void DeviceProfiler::resetControlBuffers(IDevice* device, const CoreCoord& worker_core, const ProfilerDumpState state) {
    ZoneScoped;

    chip_id_t device_id = device->id();
    HalProgrammableCoreType CoreType = tt::llrt::get_core_type(device_id, worker_core);

    const std::vector<uint32_t>& control_buffer = core_control_buffers.at(worker_core);
    std::vector<uint32_t> control_buffer_reset(kernel_profiler::PROFILER_L1_CONTROL_VECTOR_SIZE, 0);

    control_buffer_reset[kernel_profiler::DRAM_PROFILER_ADDRESS] =
        control_buffer[kernel_profiler::DRAM_PROFILER_ADDRESS];
    control_buffer_reset[kernel_profiler::FLAT_ID] = control_buffer[kernel_profiler::FLAT_ID];
    control_buffer_reset[kernel_profiler::CORE_COUNT_PER_DRAM] = control_buffer[kernel_profiler::CORE_COUNT_PER_DRAM];

    write_control_buffer_to_core(device, worker_core, CoreType, state, control_buffer_reset);
}

void DeviceProfiler::readRiscProfilerResults(
    IDevice* device,
    const CoreCoord& worker_core,
    const ProfilerDumpState state,
    const std::vector<uint32_t>& data_buffer,
    const ProfilerDataBufferSource data_source,
    const std::optional<ProfilerOptionalMetadata>& metadata,
    std::ofstream& log_file_ofs,
    nlohmann::ordered_json& noc_trace_json_log) {
    ZoneScoped;

    const std::vector<uint32_t>& control_buffer = core_control_buffers.at(worker_core);

    if ((control_buffer[kernel_profiler::HOST_BUFFER_END_INDEX_BR_ER] == 0) &&
        (control_buffer[kernel_profiler::HOST_BUFFER_END_INDEX_NC] == 0)) {
        return;
    }

    chip_id_t device_id = device->id();

    const uint32_t coreFlatID =
        tt::tt_metal::MetalContext::instance().get_cluster().get_virtual_routing_to_profiler_flat_id(device_id).at(
            worker_core);
    const uint32_t startIndex = coreFlatID * MAX_RISCV_PER_CORE * PROFILER_FULL_HOST_VECTOR_SIZE_PER_RISC;

    // translate worker core virtual coord to phys coordinates
    auto phys_coord = getPhysicalAddressFromVirtual(device_id, worker_core);

    // helper function to lookup opname from runtime id if metadata is available
    auto getOpNameIfAvailable = [&metadata](auto device_id, auto runtime_id) {
        return (metadata.has_value()) ? metadata->get_op_name(device_id, runtime_id) : "";
    };

    HalProgrammableCoreType CoreType = tt::llrt::get_core_type(device_id, worker_core);
    int riscCount = 1;

    if (CoreType == HalProgrammableCoreType::TENSIX) {
        riscCount = 5;
    }

    for (int riscEndIndex = 0; riscEndIndex < riscCount; riscEndIndex++) {
        uint32_t bufferEndIndex = control_buffer[riscEndIndex];
        if (data_source == ProfilerDataBufferSource::L1) {
            // Just grab the device end index
            bufferEndIndex = control_buffer[riscEndIndex + kernel_profiler::DEVICE_BUFFER_END_INDEX_BR_ER];
        }
        uint32_t riscType;
        if (CoreType == HalProgrammableCoreType::TENSIX) {
            riscType = riscEndIndex;
        } else {
            riscType = 5;
        }
        if (bufferEndIndex > 0) {
            uint32_t bufferRiscShift = riscEndIndex * PROFILER_FULL_HOST_VECTOR_SIZE_PER_RISC + startIndex;
            if (data_source == ProfilerDataBufferSource::L1) {
                // Shift by L1 buffer size only
                bufferRiscShift = riscEndIndex * kernel_profiler::PROFILER_L1_VECTOR_SIZE;
            }
            if ((control_buffer[kernel_profiler::DROPPED_ZONES] >> riscEndIndex) & 1) {
                std::string warningMsg = fmt::format(
                    "Profiler DRAM buffers were full, markers were dropped! device {}, worker core {}, {}, Risc "
                    "{},  "
                    "bufferEndIndex = {}. "
                    "Please either decrease the number of ops being profiled or run dump device profiler more often",
                    device_id,
                    worker_core.x,
                    worker_core.y,
                    tracy::riscName[riscEndIndex],
                    bufferEndIndex);
                TracyMessageC(warningMsg.c_str(), warningMsg.size(), tracy::Color::Tomato3);
                log_warning(tt::LogMetal, "{}", warningMsg);
            }

            uint32_t riscNumRead = 0;
            uint32_t coreFlatIDRead = 0;
            uint32_t runHostCounterRead = 0;

            bool newRunStart = false;

            uint32_t opTime_H = 0;
            uint32_t opTime_L = 0;
            std::string opname;
            for (int index = bufferRiscShift; index < (bufferRiscShift + bufferEndIndex);
                 index += kernel_profiler::PROFILER_L1_MARKER_UINT32_SIZE) {
                if (!newRunStart && data_buffer.at(index) == 0 && data_buffer.at(index + 1) == 0) {
                    newRunStart = true;
                    opTime_H = 0;
                    opTime_L = 0;
                } else if (newRunStart) {
                    newRunStart = false;

                    // TODO(MO): Cleanup magic numbers
                    riscNumRead = data_buffer.at(index) & 0x7;
                    coreFlatIDRead = (data_buffer.at(index) >> 3) & 0xFF;
                    runHostCounterRead = data_buffer.at(index + 1);

                    opname = getOpNameIfAvailable(device_id, runHostCounterRead);

                } else {
                    uint32_t timer_id = (data_buffer.at(index) >> 12) & 0x7FFFF;
                    kernel_profiler::PacketTypes packet_type = get_packet_type(timer_id);

                    switch (packet_type) {
                        case kernel_profiler::ZONE_START:
                        case kernel_profiler::ZONE_END: {
                            uint32_t time_H = data_buffer.at(index) & 0xFFF;
                            if (timer_id || time_H) {
                                uint32_t time_L = data_buffer.at(index + 1);

                                if (opTime_H == 0) {
                                    opTime_H = time_H;
                                }
                                if (opTime_L == 0) {
                                    opTime_L = time_L;
                                }

                                TT_ASSERT(
                                    riscNumRead == riscEndIndex,
                                    "Unexpected risc id, expected {}, read {}. In core {},{} {} at run {}, index {}",
                                    riscEndIndex,
                                    riscNumRead,
                                    worker_core.x,
                                    worker_core.y,
                                    magic_enum::enum_name(CoreType),
                                    runHostCounterRead,
                                    index);
                                TT_ASSERT(
                                    coreFlatIDRead == coreFlatID,
                                    "Unexpected core id, expected {}, read {}. In core {},{} {} at run {}, index {}",
                                    coreFlatID,
                                    coreFlatIDRead,
                                    worker_core.x,
                                    worker_core.y,
                                    magic_enum::enum_name(CoreType),
                                    runHostCounterRead,
                                    index);

                                logPacketData(
                                    log_file_ofs,
                                    noc_trace_json_log,
                                    runHostCounterRead,
                                    opname,
                                    device_id,
                                    phys_coord,
                                    coreFlatID,
                                    riscType,
                                    0,
                                    timer_id,
                                    (uint64_t(time_H) << 32) | time_L);
                            }
                        } break;
                        case kernel_profiler::ZONE_TOTAL: {
                            uint32_t sum = data_buffer.at(index + 1);

                            uint32_t time_H = opTime_H;
                            uint32_t time_L = opTime_L;
                            logPacketData(
                                log_file_ofs,
                                noc_trace_json_log,
                                runHostCounterRead,
                                opname,
                                device_id,
                                phys_coord,
                                coreFlatID,
                                riscType,
                                sum,
                                timer_id,
                                (uint64_t(time_H) << 32) | time_L);

                            break;
                        }
                        case kernel_profiler::TS_DATA: {
                            uint32_t time_H = data_buffer.at(index) & 0xFFF;
                            uint32_t time_L = data_buffer.at(index + 1);
                            index += kernel_profiler::PROFILER_L1_MARKER_UINT32_SIZE;
                            uint32_t data_H = data_buffer.at(index);
                            uint32_t data_L = data_buffer.at(index + 1);
                            logPacketData(
                                log_file_ofs,
                                noc_trace_json_log,
                                runHostCounterRead,
                                opname,
                                device_id,
                                phys_coord,
                                coreFlatID,
                                riscType,
                                (uint64_t(data_H) << 32) | data_L,
                                timer_id,
                                (uint64_t(time_H) << 32) | time_L);
                            continue;
                        }
                        case kernel_profiler::TS_EVENT: {
                            uint32_t time_H = data_buffer.at(index) & 0xFFF;
                            uint32_t time_L = data_buffer.at(index + 1);
                            logPacketData(
                                log_file_ofs,
                                noc_trace_json_log,
                                runHostCounterRead,
                                opname,
                                device_id,
                                phys_coord,
                                coreFlatID,
                                riscType,
                                0,
                                timer_id,
                                (uint64_t(time_H) << 32) | time_L);
                        }
                    }
                }
            }
        }
    }
}

void DeviceProfiler::firstTimestamp(uint64_t timestamp) {
    if (timestamp < smallest_timestamp) {
        smallest_timestamp = timestamp;
    }
}

ZoneDetails DeviceProfiler::getZoneDetails(uint16_t timer_id) const {
    ZoneDetails zone_details;
    auto zone_details_iter = hash_to_zone_src_locations.find(timer_id);
    if (zone_details_iter != hash_to_zone_src_locations.end()) {
        zone_details = zone_details_iter->second;
    } else {
        zone_details = UnidentifiedZoneDetails;
    }
    return zone_details;
}

void DeviceProfiler::logPacketData(
    std::ofstream& log_file_ofs,
    nlohmann::ordered_json& noc_trace_json_log,
    uint32_t run_host_id,
    const std::string& opname,
    chip_id_t device_id,
    CoreCoord core,
    int /*core_flat*/,
    int risc_num,
    uint64_t data,
    uint32_t timer_id,
    uint64_t timestamp) {
    ZoneScoped;
    kernel_profiler::PacketTypes packet_type = get_packet_type(timer_id);
    uint32_t t_id = timer_id & 0xFFFF;
    nlohmann::json metaData;

    const ZoneDetails zone_details = getZoneDetails(timer_id);

    if ((packet_type == kernel_profiler::ZONE_START) || (packet_type == kernel_profiler::ZONE_END)) {
        tracy::TTDeviceEventPhase zone_phase = tracy::TTDeviceEventPhase::begin;
        if (packet_type == kernel_profiler::ZONE_END) {
            zone_phase = tracy::TTDeviceEventPhase::end;
        }

        // TODO(MO) Until #14847 avoid attaching opID as the zone function name except for B and E FW
        // This is to avoid generating 5 to 10 times more source locations which is capped at 32K
        uint32_t tracy_run_host_id = run_host_id;
        if (!zone_details.is_zone_in_brisc_or_erisc) {
            tracy_run_host_id = 0;
        }

        auto ret = device_events.emplace(
            tracy_run_host_id,
            device_id,
            core.x,
            core.y,
            risc_num,
            timer_id,
            timestamp,
            zone_details.source_line_num,
            zone_details.source_file,
            zone_details.zone_name,
            zone_phase);
        this->current_zone_it = ret.first;

        if (!ret.second) {
            return;
        }

        device_cores.emplace(device_id, core);

        // Reset the command subtype, in case it isn't set during the command.
        this->current_dispatch_meta_data.cmd_subtype = "";
    }

    if (packet_type == kernel_profiler::TS_DATA) {
        if (this->current_zone_it != device_events.end()) {
            // Check if we are in a Tensix Dispatch zone. If so, we could have gotten dispatch meta data packets
            // These packets can amend parent zone's info
            if ((tracy::riscName[risc_num] == "BRISC" || tracy::riscName[risc_num] == "NCRISC") &&
                this->current_zone_it->zone_phase == tracy::TTDeviceEventPhase::begin &&
                this->current_zone_it->zone_name.find("DISPATCH") != std::string::npos) {
                if (zone_details.zone_name.find("process_cmd") != std::string::npos) {
                    this->current_dispatch_meta_data.cmd_type =
                        fmt::format("{}", magic_enum::enum_name((CQDispatchCmdId)data));
                    metaData["dispatch_command_type"] = this->current_dispatch_meta_data.cmd_type;
                } else if (zone_details.zone_name.find("runtime_host_id_dispatch") != std::string::npos) {
                    this->current_dispatch_meta_data.worker_runtime_id = (uint32_t)data;
                    metaData["workers_runtime_id"] = this->current_dispatch_meta_data.worker_runtime_id;
                } else if (zone_details.zone_name.find("packed_data_dispatch") != std::string::npos) {
                    this->current_dispatch_meta_data.cmd_subtype = fmt::format(
                        "{}{}",
                        data & CQ_DISPATCH_CMD_PACKED_WRITE_FLAG_MCAST ? "MCAST," : "",
                        magic_enum::enum_name(static_cast<CQDispatchCmdPackedWriteType>(
                            (data >> 1) << CQ_DISPATCH_CMD_PACKED_WRITE_TYPE_SHIFT)));
                    metaData["dispatch_command_subtype"] = this->current_dispatch_meta_data.cmd_subtype;
                } else if (zone_details.zone_name.find("packed_large_data_dispatch") != std::string::npos) {
                    this->current_dispatch_meta_data.cmd_subtype =
                        fmt::format("{}", magic_enum::enum_name(static_cast<CQDispatchCmdPackedWriteLargeType>(data)));
                    metaData["dispatch_command_subtype"] = this->current_dispatch_meta_data.cmd_subtype;
                }

                std::string newZoneName = this->current_dispatch_meta_data.cmd_type;
                if (tracy::riscName[risc_num] == "BRISC") {
                    if (this->current_dispatch_meta_data.cmd_subtype != "") {
                        newZoneName = fmt::format(
                            "{}:{}",
                            this->current_dispatch_meta_data.worker_runtime_id,
                            this->current_dispatch_meta_data.cmd_subtype);
                    } else {
                        newZoneName = fmt::format(
                            "{}:{}",
                            this->current_dispatch_meta_data.worker_runtime_id,
                            this->current_dispatch_meta_data.cmd_type);
                    }
                }
                tracy::TTDeviceEvent event = tracy::TTDeviceEvent(
                    this->current_dispatch_meta_data.worker_runtime_id,
                    this->current_zone_it->chip_id,
                    this->current_zone_it->core_x,
                    this->current_zone_it->core_y,
                    this->current_zone_it->risc,
                    this->current_zone_it->marker,
                    this->current_zone_it->timestamp,
                    this->current_zone_it->line,
                    this->current_zone_it->file,
                    newZoneName,
                    this->current_zone_it->zone_phase);
                device_events.erase(this->current_zone_it);
                auto ret = device_events.insert(event);
                this->current_zone_it = ret.first;
            }
        }
    }

    firstTimestamp(timestamp);

    logPacketDataToCSV(
        log_file_ofs,
        device_id,
        core.x,
        core.y,
        tracy::riscName[risc_num],
        t_id,
        timestamp,
        data,
        run_host_id,
        opname,
        zone_details.zone_name,
        packet_type,
        zone_details.source_line_num,
        zone_details.source_file,
        metaData);

    logNocTracePacketDataToJson(
        noc_trace_json_log,
        device_id,
        core.x,
        core.y,
        tracy::riscName[risc_num],
        t_id,
        timestamp,
        data,
        run_host_id,
        opname,
        zone_details.zone_name,
        packet_type,
        zone_details.source_line_num,
        zone_details.source_file);
}

void DeviceProfiler::logPacketDataToCSV(
    std::ofstream& log_file_ofs,
    chip_id_t device_id,
    int core_x,
    int core_y,
    const std::string_view risc_name,
    uint32_t timer_id,
    uint64_t timestamp,
    uint64_t data,
    uint32_t run_host_id,
    const std::string_view /*opname*/,
    const std::string_view zone_name,
    kernel_profiler::PacketTypes packet_type,
    uint64_t source_line,
    const std::string_view source_file,
    const nlohmann::json& metaData) {
    std::string metaDataStr = "";

    if (!metaData.is_null()) {
        metaDataStr = metaData.dump();
        std::replace(metaDataStr.begin(), metaDataStr.end(), ',', ';');
    }

    log_file_ofs << fmt::format(
        "{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
        device_id,
        core_x,
        core_y,
        risc_name,
        timer_id,
        timestamp,
        data,
        run_host_id,
        zone_name,
        magic_enum::enum_name(packet_type),
        source_line,
        source_file,
        metaDataStr);
}

void DeviceProfiler::logNocTracePacketDataToJson(
    nlohmann::ordered_json& noc_trace_json_log,
    chip_id_t device_id,
    int core_x,
    int core_y,
    const std::string_view risc_name,
    uint32_t /*timer_id*/,
    uint64_t timestamp,
    uint64_t data,
    uint32_t run_host_id,
    const std::string_view opname,
    const std::string_view zone_name,
    kernel_profiler::PacketTypes packet_type,
    uint64_t /*source_line*/,
    const std::string_view /*source_file*/) {
    if (!MetalContext::instance().rtoptions().get_profiler_noc_events_enabled()) {
        return;
    }

    if (packet_type == kernel_profiler::ZONE_START || packet_type == kernel_profiler::ZONE_END) {
        if ((risc_name == "NCRISC" || risc_name == "BRISC") &&
            (zone_name.starts_with("TRUE-KERNEL-END") || zone_name.ends_with("-KERNEL"))) {
            tracy::TTDeviceEventPhase zone_phase = (packet_type == kernel_profiler::ZONE_END)
                                                       ? tracy::TTDeviceEventPhase::end
                                                       : tracy::TTDeviceEventPhase::begin;
            noc_trace_json_log.push_back(nlohmann::ordered_json{
                {"run_host_id", run_host_id},
                {"op_name", opname},
                {"proc", risc_name},
                {"zone", zone_name},
                {"zone_phase", magic_enum::enum_name(zone_phase)},
                {"sx", core_x},
                {"sy", core_y},
                {"timestamp", timestamp},
            });
        }
    } else if (packet_type == kernel_profiler::TS_DATA) {
        using EMD = KernelProfilerNocEventMetadata;
        EMD ev_md(data);
        std::variant<EMD::LocalNocEvent, EMD::FabricNoCEvent, EMD::FabricRoutingFields> ev_md_contents =
            ev_md.getContents();
        if (std::holds_alternative<EMD::LocalNocEvent>(ev_md_contents)) {
            auto local_noc_event = std::get<EMD::LocalNocEvent>(ev_md_contents);

            // NOTE: assume here that src and dest device_id are local;
            // serialization will coalesce and update to correct destination
            // based on fabric events
            nlohmann::ordered_json data = {
                {"run_host_id", run_host_id},
                {"op_name", opname},
                {"proc", risc_name},
                {"noc", magic_enum::enum_name(local_noc_event.noc_type)},
                {"vc", int(local_noc_event.noc_vc)},
                {"src_device_id", device_id},
                {"sx", core_x},
                {"sy", core_y},
                {"num_bytes", local_noc_event.getNumBytes()},
                {"type", magic_enum::enum_name(ev_md.noc_xfer_type)},
                {"timestamp", timestamp},
            };

            // handle dst coordinates correctly for different NocEventType
            if (local_noc_event.dst_x == -1 || local_noc_event.dst_y == -1 ||
                ev_md.noc_xfer_type == EMD::NocEventType::READ_WITH_STATE ||
                ev_md.noc_xfer_type == EMD::NocEventType::WRITE_WITH_STATE) {
                // DO NOT emit destination coord; it isn't meaningful

            } else if (ev_md.noc_xfer_type == EMD::NocEventType::WRITE_MULTICAST) {
                auto phys_start_coord =
                    getPhysicalAddressFromVirtual(device_id, {local_noc_event.dst_x, local_noc_event.dst_y});
                data["mcast_start_x"] = phys_start_coord.x;
                data["mcast_start_y"] = phys_start_coord.y;
                auto phys_end_coord = getPhysicalAddressFromVirtual(
                    device_id, {local_noc_event.mcast_end_dst_x, local_noc_event.mcast_end_dst_y});
                data["mcast_end_x"] = phys_end_coord.x;
                data["mcast_end_y"] = phys_end_coord.y;
            } else {
                auto phys_coord =
                    getPhysicalAddressFromVirtual(device_id, {local_noc_event.dst_x, local_noc_event.dst_y});
                data["dx"] = phys_coord.x;
                data["dy"] = phys_coord.y;
            }

            noc_trace_json_log.push_back(std::move(data));
        } else if (std::holds_alternative<EMD::FabricNoCEvent>(ev_md_contents)) {
            EMD::FabricNoCEvent fabric_noc_event = std::get<EMD::FabricNoCEvent>(ev_md_contents);

            nlohmann::ordered_json data = {
                {"run_host_id", run_host_id},
                {"op_name", opname},
                {"proc", risc_name},
                {"sx", core_x},
                {"sy", core_y},
                {"type", magic_enum::enum_name(ev_md.noc_xfer_type)},
                {"routing_fields_type", magic_enum::enum_name(fabric_noc_event.routing_fields_type)},
                {"timestamp", timestamp},
            };

            // For scatter write operations, include additional scatter information
            if (ev_md.noc_xfer_type == EMD::NocEventType::FABRIC_UNICAST_SCATTER_WRITE) {
                data["scatter_address_index"] = fabric_noc_event.mcast_end_dst_x;
                data["scatter_total_addresses"] = fabric_noc_event.mcast_end_dst_y;
            }

            // handle dst coordinates correctly for different NocEventType
            if (KernelProfilerNocEventMetadata::isFabricUnicastEventType(ev_md.noc_xfer_type)) {
                auto phys_coord =
                    getPhysicalAddressFromVirtual(device_id, {fabric_noc_event.dst_x, fabric_noc_event.dst_y});
                data["dx"] = phys_coord.x;
                data["dy"] = phys_coord.y;
            } else {
                auto phys_start_coord =
                    getPhysicalAddressFromVirtual(device_id, {fabric_noc_event.dst_x, fabric_noc_event.dst_y});
                data["mcast_start_x"] = phys_start_coord.x;
                data["mcast_start_y"] = phys_start_coord.y;
                auto phys_end_coord = getPhysicalAddressFromVirtual(
                    device_id, {fabric_noc_event.mcast_end_dst_x, fabric_noc_event.mcast_end_dst_y});
                data["mcast_end_x"] = phys_end_coord.x;
                data["mcast_end_y"] = phys_end_coord.y;
            }

            noc_trace_json_log.push_back(std::move(data));
        } else if (std::holds_alternative<EMD::FabricRoutingFields>(ev_md_contents)) {
            uint32_t routing_fields_value = std::get<EMD::FabricRoutingFields>(ev_md_contents).routing_fields_value;
            noc_trace_json_log.push_back(nlohmann::ordered_json{
                {"run_host_id", run_host_id},
                {"op_name", opname},
                {"proc", risc_name},
                {"sx", core_x},
                {"sy", core_y},
                {"routing_fields_value", routing_fields_value},
                {"timestamp", timestamp},
            });
        }
    }
}

void DeviceProfiler::emitCSVHeader(
    std::ofstream& log_file_ofs, const tt::ARCH& device_architecture, int device_core_frequency) const {
    log_file_ofs << "ARCH: " << get_string_lowercase(device_architecture)
                 << ", CHIP_FREQ[MHz]: " << device_core_frequency << std::endl;
    log_file_ofs << "PCIe slot, core_x, core_y, RISC processor type, timer_id, time[cycles since reset], data, "
                    "run host ID,  zone name, type, source line, source file, meta data"
                 << std::endl;
}

void DeviceProfiler::serializeJsonNocTraces(
    const nlohmann::ordered_json& noc_trace_json_log,
    const std::filesystem::path& output_dir,
    chip_id_t device_id,
    const FabricRoutingLookup& routing_lookup) {
    // create output directory if it does not exist
    std::filesystem::create_directories(output_dir);
    if (!std::filesystem::is_directory(output_dir)) {
        log_error(
            tt::LogMetal,
            "Could not write noc event json trace to '{}' because the directory path could not be created!",
            output_dir);
        return;
    }

    // bin events by runtime id
    using RuntimeID = uint32_t;
    std::unordered_map<RuntimeID, nlohmann::json::array_t> events_by_opname;
    for (auto& json_event : noc_trace_json_log) {
        RuntimeID runtime_id = json_event.value("run_host_id", -1);
        events_by_opname[runtime_id].push_back(json_event);
    }

    // sort events in each opname group by proc first, then timestamp
    for (auto& [runtime_id, events] : events_by_opname) {
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
            auto sx_a = a.value("sx", 0);
            auto sy_a = a.value("sy", 0);
            auto sx_b = b.value("sx", 0);
            auto sy_b = b.value("sy", 0);
            auto proc_a = a.value("proc", "");
            auto proc_b = b.value("proc", "");
            auto timestamp_a = a.value("timestamp", 0);
            auto timestamp_b = b.value("timestamp", 0);
            return std::tie(sx_a, sy_a, proc_a, timestamp_a) < std::tie(sx_b, sy_b, proc_b, timestamp_b);
        });
    }

    // for each opname in events_by_opname, adjust timestamps to be relative to the smallest timestamp within the
    // group with identical sx,sy,proc
    for (auto& [runtime_id, events] : events_by_opname) {
        std::tuple<int, int, std::string> reference_event_loc;
        uint64_t reference_timestamp = 0;
        for (auto& event : events) {
            std::string zone = event.value("zone", "");
            std::string zone_phase = event.value("zone_phase", "");
            uint64_t curr_timestamp = event.value("timestamp", 0);
            // if -KERNEL::begin event is found, reset the reference timestamp
            if (zone.ends_with("-KERNEL") && zone_phase == "begin") {
                reference_timestamp = curr_timestamp;
            }

            // fix timestamp to be relative to reference_timestamp
            event["timestamp"] = curr_timestamp - reference_timestamp;
        }
    }

    auto process_fabric_event_group_if_valid =
        [&](const nlohmann::ordered_json& fabric_event,
            const nlohmann::ordered_json& fabric_routing_fields_event,
            const nlohmann::ordered_json& local_noc_write_event) -> std::optional<nlohmann::ordered_json> {
        bool local_event_is_valid_type =
            local_noc_write_event.contains("type") && local_noc_write_event["type"] == "WRITE_";
        if (!local_event_is_valid_type) {
            log_error(
                tt::LogMetal,
                "[profiler noc tracing] local noc event following fabric event is not a regular noc write, but instead "
                ": {}",
                local_noc_write_event["type"].get<std::string>());
            return std::nullopt;
        }

        // Check if timestamps are close enough; otherwise
        double ts_diff = local_noc_write_event.value("timestamp", 0.0) - fabric_event.value("timestamp", 0.0);
        if (ts_diff > 1000) {
            log_warning(
                tt::LogMetal,
                "[profiler noc tracing] Failed to coalesce fabric noc trace events because timestamps are implausibly "
                "far apart.");
            return std::nullopt;
        }

        try {
            // router eth core location is derived from the following noc WRITE_ event
            CoreCoord virt_eth_route_coord = {
                local_noc_write_event.at("dx").get<int>(), local_noc_write_event.at("dy").get<int>()};
            CoreCoord phys_eth_route_coord = getPhysicalAddressFromVirtual(device_id, virt_eth_route_coord);

            auto routing_fields_type_str = fabric_event.at("routing_fields_type").get<std::string>();
            auto maybe_routing_fields_type =
                magic_enum::enum_cast<KernelProfilerNocEventMetadata::FabricPacketType>(routing_fields_type_str);
            if (!maybe_routing_fields_type) {
                log_error(
                    tt::LogMetal,
                    "[profiler noc tracing] Failed to parse routing fields type: {}",
                    routing_fields_type_str);
                return std::nullopt;
            }
            auto routing_fields_type = maybe_routing_fields_type.value();

            // determine hop count and other routing metadata from routing fields value
            uint32_t routing_fields_value = fabric_routing_fields_event.at("routing_fields_value").get<uint32_t>();
            int start_distance = 0;
            int range = 0;
            switch (routing_fields_type) {
                case KernelProfilerNocEventMetadata::FabricPacketType::REGULAR: {
                    std::tie(start_distance, range) = get_routing_start_distance_and_range(routing_fields_value);
                    break;
                }
                case KernelProfilerNocEventMetadata::FabricPacketType::LOW_LATENCY: {
                    std::tie(start_distance, range) =
                        get_low_latency_routing_start_distance_and_range(routing_fields_value);
                    break;
                }
                case KernelProfilerNocEventMetadata::FabricPacketType::LOW_LATENCY_MESH: {
                    log_error(
                        tt::LogMetal, "[profiler noc tracing] noc tracing does not support LOW_LATENCY_MESH packets!");
                    return std::nullopt;
                }
            }

            auto eth_chan_opt = routing_lookup.getRouterEthCoreToChannelLookup(device_id, phys_eth_route_coord);
            if (!eth_chan_opt) {
                log_warning(
                    tt::LogMetal,
                    "[profiler noc tracing] Fabric edm_location->channel lookup failed for event in op '{}' at ts {}: "
                    "src_dev={}, "
                    "eth_core=({}, {}), start_distance={}. Keeping original events.",
                    fabric_event.value("op_name", "N/A"),
                    fabric_event.value("timestamp", 0.0),
                    device_id,
                    phys_eth_route_coord.x,
                    phys_eth_route_coord.y,
                    start_distance);
                return std::nullopt;
            }

            tt::tt_fabric::chan_id_t eth_chan = *eth_chan_opt;

            nlohmann::ordered_json modified_write_event = local_noc_write_event;
            modified_write_event["timestamp"] = fabric_event["timestamp"];

            // replace original eth core destination with true destination
            auto noc_xfer_type = magic_enum::enum_cast<KernelProfilerNocEventMetadata::NocEventType>(
                fabric_event["type"].get<std::string>());

            if (!noc_xfer_type.has_value() ||
                !KernelProfilerNocEventMetadata::isFabricEventType(noc_xfer_type.value())) {
                log_error(
                    tt::LogMetal,
                    "[profiler noc tracing] Failed to parse noc transfer type: {}",
                    fabric_event["type"].get<std::string>());
                return std::nullopt;
            }

            if (KernelProfilerNocEventMetadata::isFabricUnicastEventType(noc_xfer_type.value())) {
                modified_write_event["dx"] = fabric_event.at("dx").get<int>();
                modified_write_event["dy"] = fabric_event.at("dy").get<int>();
            } else {
                log_error(tt::LogMetal, "[profiler noc tracing] Noc multicasts in fabric events are not supported!");
                return std::nullopt;
            }

            // replace the type with fabric event type
            modified_write_event["type"] = fabric_event["type"];

            modified_write_event["fabric_send"] = {
                {"eth_chan", eth_chan}, {"start_distance", start_distance}, {"range", range}};

            return modified_write_event;
        } catch (const nlohmann::json::exception& e) {
            log_warning(
                tt::LogMetal,
                "[profiler noc tracing] JSON parsing error during event coalescing for event in op '{}': {}",
                fabric_event.value("op_name", "N/A"),
                e.what());
            return std::nullopt;
        }
    };

    // coalesce fabric events into single logical trace events with extra 'fabric_send' metadata
    std::unordered_map<RuntimeID, nlohmann::json::array_t> processed_events_by_opname;
    for (auto& [runtime_id, events] : events_by_opname) {
        nlohmann::json::array_t coalesced_events;
        for (size_t i = 0; i < events.size(); /* manual increment */) {
            const auto& current_event = events[i];

            bool fabric_event_group_detected =
                (current_event.contains("type") && current_event["type"].get<std::string>().starts_with("FABRIC_") &&
                 (i + 2 < events.size()));
            if (fabric_event_group_detected) {
                if (auto maybe_fabric_event =
                        process_fabric_event_group_if_valid(events[i], events[i + 1], events[i + 2]);
                    maybe_fabric_event) {
                    coalesced_events.push_back(maybe_fabric_event.value());
                }
                // Unconditionally advance past all coalesced events (fabric_event, fabric_routing_fields,
                // local_noc_write_event), even if a valid event cannot be generated
                i += 3;
            } else {
                // If not a fabric event group, simply copy existing event as-is
                coalesced_events.push_back(current_event);
                i += 1;
            }
        }
        // Store the final coalesced/processed list for this op_name
        processed_events_by_opname[runtime_id] = std::move(coalesced_events);
    }

    log_info(tt::LogMetal, "Writing profiler noc traces to '{}'", output_dir);
    for (auto& [runtime_id, events] : processed_events_by_opname) {
        // dump events to a json file inside directory output_dir named after the opname
        std::filesystem::path rpt_path = output_dir;
        std::string op_name = events.front().value("op_name", "UnknownOP");
        if (!op_name.empty()) {
            rpt_path /= fmt::format("noc_trace_dev{}_{}_ID{}.json", device_id, op_name, runtime_id);
        } else {
            rpt_path /= fmt::format("noc_trace_dev{}_ID{}.json", device_id, runtime_id);
        }
        std::ofstream file(rpt_path);
        if (file.is_open()) {
            // Write the final processed events for this op
            file << nlohmann::json(std::move(events)).dump(2);
        } else {
            log_error(tt::LogMetal, "Could not write noc event json trace to '{}'", rpt_path);
        }
    }
}

CoreCoord DeviceProfiler::getPhysicalAddressFromVirtual(chip_id_t device_id, const CoreCoord& c) const {
    bool coord_is_translated = c.x >= MetalContext::instance().hal().get_virtual_worker_start_x() - 1 ||
                               c.y >= MetalContext::instance().hal().get_virtual_worker_start_y() - 1;
    try {
        if (MetalContext::instance().hal().is_coordinate_virtualization_enabled() && coord_is_translated) {
            const metal_SocDescriptor& soc_desc =
                tt::tt_metal::MetalContext::instance().get_cluster().get_soc_desc(device_id);
            // disable linting here; slicing is __intended__
            // NOLINTBEGIN
            return soc_desc.translate_coord_to(c, CoordSystem::TRANSLATED, CoordSystem::PHYSICAL);
            // NOLINTEND
        } else {
            return c;
        }
    } catch (const std::exception& e) {
        log_error(tt::LogMetal, "Failed to translate virtual coordinate {},{} to physical", c.x, c.y);
        return c;
    }
    return c;
}

DeviceProfiler::DeviceProfiler(const IDevice* device, const bool new_logs) {
#if defined(TRACY_ENABLE)
    ZoneScopedC(tracy::Color::Green);
    output_dir = std::filesystem::path(get_profiler_logs_dir());
    std::filesystem::create_directories(output_dir);
    std::filesystem::path log_path = output_dir / DEVICE_SIDE_LOG;

    if (new_logs) {
        std::filesystem::remove(log_path);
    }

    this->current_zone_it = device_events.begin();
    device_sync_info = std::make_tuple(0.0, 0.0, 0.0);
    device_events.reserve(
        (MAX_RISCV_PER_CORE * PROFILER_FULL_HOST_VECTOR_SIZE_PER_RISC * device->compute_with_storage_grid_size().x *
         device->compute_with_storage_grid_size().y) /
        kernel_profiler::PROFILER_L1_MARKER_UINT32_SIZE);
    device_cores.reserve(device->compute_with_storage_grid_size().x * device->compute_with_storage_grid_size().y);
#endif
}

DeviceProfiler::~DeviceProfiler() {
#if defined(TRACY_ENABLE)
    ZoneScoped;
    pushTracyDeviceResults();
    for (auto tracyCtx : device_tracy_contexts) {
        TracyTTDestroy(tracyCtx.second);
    }
#endif
}

void DeviceProfiler::freshDeviceLog() {
#if defined(TRACY_ENABLE)
    std::filesystem::path log_path = output_dir / DEVICE_SIDE_LOG;
    std::filesystem::remove(log_path);
#endif
}

void DeviceProfiler::setOutputDir(const std::string& new_output_dir) {
#if defined(TRACY_ENABLE)
    std::filesystem::create_directories(new_output_dir);
    output_dir = new_output_dir;
#endif
}

void DeviceProfiler::setDeviceArchitecture(tt::ARCH device_arch) {
#if defined(TRACY_ENABLE)
    device_architecture = device_arch;
#endif
}

uint32_t DeviceProfiler::hash32CT(const char* str, size_t n, uint32_t basis) {
    return n == 0 ? basis : hash32CT(str + 1, n - 1, (basis ^ str[0]) * UINT32_C(16777619));
}

uint16_t DeviceProfiler::hash16CT(const std::string& str) {
    uint32_t res = hash32CT(str.c_str(), str.length());
    return ((res & 0xFFFF) ^ ((res & 0xFFFF0000) >> 16)) & 0xFFFF;
}

void DeviceProfiler::generateZoneSourceLocationsHashes() {
    std::ifstream log_file(tt::tt_metal::PROFILER_ZONE_SRC_LOCATIONS_LOG);
    std::string line;
    while (std::getline(log_file, line)) {
        std::string delimiter = "'#pragma message: ";
        int delimiter_index = line.find(delimiter) + delimiter.length();
        std::string zone_src_location = line.substr(delimiter_index, line.length() - delimiter_index - 1);

        uint16_t hash_16bit = hash16CT(zone_src_location);

        auto did_insert = zone_src_locations.insert(zone_src_location);
        if (did_insert.second && (hash_to_zone_src_locations.find(hash_16bit) != hash_to_zone_src_locations.end())) {
            log_warning(
                tt::LogAlways,
                "Source location hashes are colliding, two different locations are having the same hash");
        }

        ZoneDetails details;
        std::stringstream ss(zone_src_location);
        std::getline(ss, details.zone_name, ',');
        std::getline(ss, details.source_file, ',');
        std::string line_num_str;
        std::getline(ss, line_num_str, ',');
        details.source_line_num = std::stoull(line_num_str);
        details.is_zone_in_brisc_or_erisc =
            (details.zone_name.find("BRISC-FW") != std::string::npos ||
             details.zone_name.find("ERISC-FW") != std::string::npos);

        hash_to_zone_src_locations.emplace(hash_16bit, details);
    }
}

void DeviceProfiler::dumpResults(
    IDevice* device,
    const std::vector<CoreCoord>& worker_cores,
    const ProfilerDumpState state,
    const ProfilerDataBufferSource data_source,
    const std::optional<ProfilerOptionalMetadata>& metadata) {
#if defined(TRACY_ENABLE)
    ZoneScoped;

    const chip_id_t device_id = device->id();
    const auto& rtoptions = tt::tt_metal::MetalContext::instance().rtoptions();
    device_core_frequency = tt::tt_metal::MetalContext::instance().get_cluster().get_device_aiclk(device_id);

    generateZoneSourceLocationsHashes();

    FabricRoutingLookup routing_lookup;
    if (state == ProfilerDumpState::NORMAL && rtoptions.get_profiler_noc_events_enabled()) {
        routing_lookup = FabricRoutingLookup(device);
    }

    if (data_source == ProfilerDataBufferSource::DRAM) {
        for (const auto& worker_core : worker_cores) {
            readControlBuffers(device, worker_core, state);
        }

        if (tt::DevicePool::instance().is_dispatch_firmware_active()) {
            if (useSlowDispatchForReading(state)) {
                issueSlowDispatchReadFromProfilerBuffer(device);
            } else {
                issueFastDispatchReadFromProfilerBuffer(device);
            }
        } else {
            issueSlowDispatchReadFromProfilerBuffer(device);
        }
        for (const auto& worker_core : worker_cores) {
            resetControlBuffers(device, worker_core, state);
        }
    }

    const std::string zone_name =
        fmt::format("{}-{}-{}", device_id, magic_enum::enum_name(state), magic_enum::enum_name(data_source));
    ZoneName(zone_name.c_str(), zone_name.size());

    if (rtoptions.get_profiler_noc_events_enabled()) {
        log_warning(
            tt::LogAlways, "Profiler NoC events are enabled; this can add 1-15% cycle overhead to typical operations!");
    }

    // open CSV log file
    std::filesystem::path log_path = output_dir / DEVICE_SIDE_LOG;
    std::ofstream log_file_ofs;

    // append to existing CSV log file if it already exists
    if (std::filesystem::exists(log_path)) {
        log_file_ofs.open(log_path, std::ios_base::app);
    } else {
        log_file_ofs.open(log_path);
        emitCSVHeader(log_file_ofs, device_architecture, device_core_frequency);
    }

    // create nlohmann json log object
    nlohmann::ordered_json noc_trace_json_log = nlohmann::json::array();

    if (!log_file_ofs) {
        log_error(tt::LogMetal, "Could not open kernel profiler dump file '{}'", log_path);
    } else {
        for (const auto& worker_core : worker_cores) {
            if (data_source == ProfilerDataBufferSource::L1) {
                readControlBuffers(device, worker_core, state);
                resetControlBuffers(device, worker_core, state);

                std::vector<uint32_t> core_l1_data_buffer;
                if (tt::DevicePool::instance().is_dispatch_firmware_active()) {
                    if (useSlowDispatchForReading(state)) {
                        core_l1_data_buffer = issueSlowDispatchReadFromL1DataBuffer(device, worker_core);
                    } else {
                        core_l1_data_buffer = issueFastDispatchReadFromL1DataBuffer(device, worker_core);
                    }
                } else {
                    core_l1_data_buffer = issueSlowDispatchReadFromL1DataBuffer(device, worker_core);
                }

                readRiscProfilerResults(
                    device,
                    worker_core,
                    state,
                    core_l1_data_buffer,
                    ProfilerDataBufferSource::L1,
                    metadata,
                    log_file_ofs,
                    noc_trace_json_log);
            } else {
                readRiscProfilerResults(
                    device,
                    worker_core,
                    state,
                    profile_buffer,
                    ProfilerDataBufferSource::DRAM,
                    metadata,
                    log_file_ofs,
                    noc_trace_json_log);
            }
        }

        // if defined, used profiler_noc_events_report_path to write json log. otherwise use output_dir
        std::string rpt_path = rtoptions.get_profiler_noc_events_report_path();
        if (rpt_path.empty()) {
            rpt_path = output_dir.string();
        }

        // serialize noc traces only in normal state, to avoid overwriting individual trace files
        if (state == ProfilerDumpState::NORMAL && rtoptions.get_profiler_noc_events_enabled()) {
            serializeJsonNocTraces(noc_trace_json_log, rpt_path, device_id, routing_lookup);
            dumpClusterCoordinatesAsJson(std::filesystem::path(rpt_path) / "cluster_coordinates.json");
        }

        log_file_ofs.close();
    }
#endif
}

bool isSyncInfoNewer(
    const std::tuple<double, double, double>& old_info, const std::tuple<double, double, double>& new_info) {
    double old_cpu_time = get<0>(old_info);
    double old_device_time = get<1>(old_info);
    double old_frequency = get<2>(old_info);
    double new_cpu_time = get<0>(new_info);
    double new_device_time = get<1>(new_info);
    double new_frequency = get<2>(new_info);
    return (
        (old_frequency == 0 && new_frequency != 0) ||
        (old_cpu_time < new_cpu_time) && ((old_device_time / old_frequency) < (new_device_time / new_frequency)));
}

void sortDeviceEvents(std::vector<std::reference_wrapper<const tracy::TTDeviceEvent>>& device_events) {
    constexpr uint32_t num_threads = 8;

    if (device_events.size() < num_threads) {
        std::sort(
            device_events.begin(),
            device_events.end(),
            [](std::reference_wrapper<const tracy::TTDeviceEvent> a,
               std::reference_wrapper<const tracy::TTDeviceEvent> b) { return a.get() < b.get(); });
        return;
    }

    std::array<std::thread, num_threads - 1> threads;
    const uint32_t chunk_size = device_events.size() / num_threads;
    for (uint32_t i = 0; i < num_threads - 1; i++) {
        const uint32_t start_idx = i * chunk_size;
        const uint32_t end_idx = (i + 1) * chunk_size;
        threads[i] = std::thread([&device_events, start_idx, end_idx]() {
            std::sort(
                device_events.begin() + start_idx,
                device_events.begin() + end_idx,
                [](std::reference_wrapper<const tracy::TTDeviceEvent> a,
                   std::reference_wrapper<const tracy::TTDeviceEvent> b) { return a.get() < b.get(); });
        });
    }

    std::sort(
        device_events.begin() + (num_threads - 1) * chunk_size,
        device_events.end(),
        [](std::reference_wrapper<const tracy::TTDeviceEvent> a, std::reference_wrapper<const tracy::TTDeviceEvent> b) {
            return a.get() < b.get();
        });

    for (auto& thread : threads) {
        thread.join();
    }

    uint32_t chunk_idx = 0;
    for (uint32_t i = 0; i < (num_threads / 2) - 1; ++i) {
        threads[i] = std::thread([&device_events, chunk_size, chunk_idx]() {
            std::inplace_merge(
                device_events.begin() + chunk_idx * chunk_size,
                device_events.begin() + (chunk_idx + 1) * chunk_size,
                device_events.begin() + (chunk_idx + 2) * chunk_size,
                [](std::reference_wrapper<const tracy::TTDeviceEvent> a,
                   std::reference_wrapper<const tracy::TTDeviceEvent> b) { return a.get() < b.get(); });
        });
        chunk_idx += 2;
    }

    std::inplace_merge(
        device_events.begin() + chunk_idx * chunk_size,
        device_events.begin() + (chunk_idx + 1) * chunk_size,
        device_events.end(),
        [](std::reference_wrapper<const tracy::TTDeviceEvent> a, std::reference_wrapper<const tracy::TTDeviceEvent> b) {
            return a.get() < b.get();
        });

    for (uint32_t i = 0; i < (num_threads / 2) - 1; ++i) {
        threads[i].join();
    }

    chunk_idx = 0;
    for (uint32_t i = 0; i < (num_threads / 4) - 1; ++i) {
        threads[i] = std::thread([&device_events, chunk_size, chunk_idx]() {
            std::inplace_merge(
                device_events.begin() + chunk_idx * chunk_size,
                device_events.begin() + (chunk_idx + 2) * chunk_size,
                device_events.begin() + (chunk_idx + 4) * chunk_size,
                [](std::reference_wrapper<const tracy::TTDeviceEvent> a,
                   std::reference_wrapper<const tracy::TTDeviceEvent> b) { return a.get() < b.get(); });
        });
        chunk_idx += 4;
    }

    std::inplace_merge(
        device_events.begin() + chunk_idx * chunk_size,
        device_events.begin() + (chunk_idx + 2) * chunk_size,
        device_events.end(),
        [](std::reference_wrapper<const tracy::TTDeviceEvent> a, std::reference_wrapper<const tracy::TTDeviceEvent> b) {
            return a.get() < b.get();
        });

    for (uint32_t i = 0; i < (num_threads / 4) - 1; ++i) {
        threads[i].join();
    }

    std::inplace_merge(
        device_events.begin(),
        device_events.begin() + 4 * chunk_size,
        device_events.end(),
        [](std::reference_wrapper<const tracy::TTDeviceEvent> a, std::reference_wrapper<const tracy::TTDeviceEvent> b) {
            return a.get() < b.get();
        });

    TT_ASSERT(std::is_sorted(
        device_events.begin(),
        device_events.end(),
        [](std::reference_wrapper<const tracy::TTDeviceEvent> a, std::reference_wrapper<const tracy::TTDeviceEvent> b) {
            return a.get() < b.get();
        }));
}

std::vector<std::reference_wrapper<const tracy::TTDeviceEvent>> getDeviceEventsVector(
    const std::unordered_set<tracy::TTDeviceEvent>& device_events) {
    tracy::TTDeviceEvent dummy_event;
    std::vector<std::reference_wrapper<const tracy::TTDeviceEvent>> device_events_vec(
        device_events.size(), std::cref(dummy_event));

    auto middle = device_events.begin();
    std::advance(middle, device_events.size() / 2);

    std::thread t([&device_events_vec, &device_events, &middle]() {
        uint32_t i = device_events.size() / 2;
        for (auto it = middle; it != device_events.end(); ++it) {
            device_events_vec[i] = std::cref(*it);
            i++;
        }
    });

    uint32_t i = 0;
    for (auto it = device_events.begin(); it != middle; ++it) {
        device_events_vec[i] = std::cref(*it);
        i++;
    }

    t.join();

    return device_events_vec;
}

void DeviceProfiler::pushTracyDeviceResults() {
#if defined(TRACY_ENABLE)
    ZoneScoped;

    // If this device is root, it may have new sync info updated with syncDeviceHost
    // called during DumpDeviceProfilerResults
    for (auto& [core, info] : device_core_sync_info) {
        if (isSyncInfoNewer(device_sync_info, info)) {
            setSyncInfo(info);
        }
    }

    // IMPORTANT: This function creates a vector of references to the TTDeviceEvent objects stored in the device_events
    // unordered set. These are direct references to the original objects, not copies of the data.
    // Thread safety warning: The device_events set MUST NOT be modified (no insertions, deletions, or rehashing) while
    // these references are in use, as this could invalidate the references and cause undefined behavior.
    std::vector<std::reference_wrapper<const tracy::TTDeviceEvent>> device_events_vec =
        getDeviceEventsVector(device_events);

    sortDeviceEvents(device_events_vec);

    // Tracy contexts must be updated in order of their first timestamps
    for (auto& event : device_events_vec) {
        auto device_core_it = device_cores.find({event.get().chip_id, {event.get().core_x, event.get().core_y}});
        if (device_core_it != device_cores.end()) {
            updateTracyContext(*device_core_it);
            device_cores.erase(device_core_it);
        }

        if (device_cores.empty()) {
            break;
        }
    }

    for (auto& event : device_events_vec) {
        std::reference_wrapper<const tracy::TTDeviceEvent> event_to_push = event;

        // Using std::optional to avoid calling the default constructor of tracy::TTDeviceEvent
        std::optional<tracy::TTDeviceEvent> event_with_adjusted_timestamp;
        const uint64_t adjusted_timestamp = event.get().timestamp * this->freqScale + this->shift;
        if (adjusted_timestamp != event.get().timestamp) {
            event_with_adjusted_timestamp.emplace(
                event.get().run_num,
                event.get().chip_id,
                event.get().core_x,
                event.get().core_y,
                event.get().risc,
                event.get().marker,
                adjusted_timestamp,
                event.get().line,
                event.get().file,
                event.get().zone_name,
                event.get().zone_phase);
            event_to_push = std::cref(event_with_adjusted_timestamp.value());
        }

        std::pair<chip_id_t, CoreCoord> device_core = {
            event_to_push.get().chip_id, (CoreCoord){event_to_push.get().core_x, event_to_push.get().core_y}};
        if (event_to_push.get().zone_phase == tracy::TTDeviceEventPhase::begin) {
            TracyTTPushStartZone(device_tracy_contexts[device_core], event_to_push.get());
        } else if (event_to_push.get().zone_phase == tracy::TTDeviceEventPhase::end) {
            TracyTTPushEndZone(device_tracy_contexts[device_core], event_to_push.get());
        }
    }

    device_events.clear();
#endif
}

void DeviceProfiler::setSyncInfo(const std::tuple<double, double, double>& sync_info) { device_sync_info = sync_info; }

void DeviceProfiler::updateTracyContext(std::pair<uint32_t, CoreCoord> device_core) {
#if defined(TRACY_ENABLE)
    chip_id_t device_id = device_core.first;
    CoreCoord worker_core = device_core.second;

    if (device_tracy_contexts.find(device_core) == device_tracy_contexts.end()) {
        // Create a new tracy context for this device core
        auto tracyCtx = TracyTTContext();
        std::string tracyTTCtxName = fmt::format("Device: {}, Core ({},{})", device_id, worker_core.x, worker_core.y);

        double cpu_time = get<0>(device_sync_info);
        double device_time = get<1>(device_sync_info);
        double frequency = get<2>(device_sync_info);

        if (frequency == 0) {
            cpu_time = TracyGetCpuTime();
            device_time = smallest_timestamp;
            frequency = device_core_frequency / 1000.0;
            device_sync_info = std::make_tuple(cpu_time, device_time, frequency);
            log_debug(
                tt::LogMetal,
                "For device {}, core {},{} default frequency was used and its zones will be out of sync",
                device_id,
                worker_core.x,
                worker_core.y);
        } else {
            log_debug(
                tt::LogMetal,
                "Device {}, core {},{} sync info are, frequency {} GHz,  delay {} cycles and, sync point {} seconds",
                device_id,
                worker_core.x,
                worker_core.y,
                frequency,
                device_time,
                cpu_time);
        }

        TracyTTContextPopulate(tracyCtx, cpu_time, device_time, frequency);

        TracyTTContextName(tracyCtx, tracyTTCtxName.c_str(), tracyTTCtxName.size());

        device_tracy_contexts.emplace(device_core, tracyCtx);
        core_sync_info[worker_core] = std::make_tuple(cpu_time, device_time, frequency);
    } else {
        // Update the existing tracy context for this device core
        if (isSyncInfoNewer(core_sync_info[worker_core], device_sync_info)) {
            core_sync_info[worker_core] = device_sync_info;
            double cpu_time = get<0>(device_sync_info);
            double device_time = get<1>(device_sync_info);
            double frequency = get<2>(device_sync_info);
            auto tracyCtx = device_tracy_contexts.at(device_core);
            TracyTTContextCalibrate(tracyCtx, cpu_time, device_time, frequency);
            log_debug(
                tt::LogMetal,
                "Device {}, core {},{} calibration info are, frequency {} GHz,  delay {} cycles and, sync point {} "
                "seconds",
                device_id,
                worker_core.x,
                worker_core.y,
                frequency,
                device_time,
                cpu_time);
        }
    }
#endif
}

bool getDeviceProfilerState() { return tt::tt_metal::MetalContext::instance().rtoptions().get_profiler_enabled(); }

bool onlyProfileDispatchCores(ProfilerDumpState state) {
    return tt::tt_metal::MetalContext::instance().rtoptions().get_profiler_do_dispatch_cores() &&
           state == ProfilerDumpState::ONLY_DISPATCH_CORES;
}

}  // namespace tt_metal

}  // namespace tt
