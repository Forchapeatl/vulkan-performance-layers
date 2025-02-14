// Copyright 2020-2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "layer_data.h"

#include <cinttypes>
#include <cstdint>
#include <iomanip>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "debug_logging.h"
#include "layer_utils.h"

namespace performancelayers {
namespace {
constexpr char kEventLogFileEnvVar[] = "VK_PERFORMANCE_LAYERS_EVENT_LOG_FILE";

// Returns event log file row prefix with ','-separated |event_type| and
// |timestamp|.
std::string MakeEventLogPrefix(
    std::string_view event_type,
    TimestampClock::time_point timestamp = GetTimestamp()) {
  return CsvCat(event_type, ToUnixNanos(timestamp));
}

// Returns the first create info of type
// VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO in the chain |create_info|.
// Returns |nullptr| if no info is found.
VkLayerInstanceCreateInfo* FindInstanceCreateInfo(
    const VkInstanceCreateInfo* create_info) {
  auto* instance_create_info = const_cast<VkLayerInstanceCreateInfo*>(
      static_cast<const VkLayerInstanceCreateInfo*>(create_info->pNext));

  while (instance_create_info &&
         (instance_create_info->sType !=
              VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
          instance_create_info->function != VK_LAYER_LINK_INFO)) {
    instance_create_info = const_cast<VkLayerInstanceCreateInfo*>(
        static_cast<const VkLayerInstanceCreateInfo*>(
            instance_create_info->pNext));
  }
  return instance_create_info;
}

// Returns the first create info of type
// VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO in the chain |create_info|.
// Returns |nullptr| if no info is found.
VkLayerDeviceCreateInfo* FindDeviceCreateInfo(
    const VkDeviceCreateInfo* create_info) {
  auto* device_create_info = const_cast<VkLayerDeviceCreateInfo*>(
      static_cast<const VkLayerDeviceCreateInfo*>(create_info->pNext));

  while (device_create_info &&
         (device_create_info->sType !=
              VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
          device_create_info->function != VK_LAYER_LINK_INFO)) {
    device_create_info = const_cast<VkLayerDeviceCreateInfo*>(
        static_cast<const VkLayerDeviceCreateInfo*>(device_create_info->pNext));
  }

  return device_create_info;
}
}  // namespace

LayerData::LayerData() {
  if (const char* event_log_file = getenv(kEventLogFileEnvVar)) {
    // The underlying log file can be written to by multiple layers from
    // multiple threads. All contentens have to be written in whole lines(s)
    // at a time to ensure there is no unintended interleaving within a single
    // line.
    event_log_ = fopen(event_log_file, "a");
  }
}

LayerData::LayerData(char* log_filename, const char* header) {
  if (log_filename) {
    out_ = fopen(log_filename, "w");
    if (out_ == nullptr) {
      SPL_LOG(ERROR) << "Failed to open " << log_filename
                     << ", output will be to STDERR.";
      out_ = stderr;
    }
  } else {
    out_ = stderr;
  }

  WriteLnAndFlush(out_, header);

  if (const char* event_log_file = getenv(kEventLogFileEnvVar)) {
    // The underlying log file can be written to by multiple layers from
    // multiple threads. All contentens have to be written in whole lines(s)
    // at a time to ensure there is no unintended interleaving within a single
    // line.
    event_log_ = fopen(event_log_file, "a");
  }
}

void LayerData::RemoveInstance(VkInstance instance) {
  InstanceKey key(instance);
  absl::MutexLock lock(&instance_dispatch_lock_);
  instance_dispatch_map_.erase(key);
  instance_keys_map_.erase(key);
}

void LayerData::LogLine(std::string_view event_type, std::string_view line,
                        TimestampClock::time_point timestamp) const {
  WriteLnAndFlush(out_, line);
  if (event_log_)
    WriteLnAndFlush(event_log_,
                    CsvCat(MakeEventLogPrefix(event_type, timestamp), line));
}

void LayerData::Log(std::string_view event_type, const HashVector& pipeline,
                    std::string_view prefix) const {
  // Quote the comma-separated hash value array to always create 2 CSV cells.
  std::stringstream pipeline_hash_str;
  pipeline_hash_str << std::quoted(PipelineHashToString(pipeline));
  std::string pipeline_and_content = CsvCat(pipeline_hash_str.str(), prefix);
  LogLine(event_type, pipeline_and_content);
}

DurationClock::duration LayerData::GetTimeDelta() {
  absl::MutexLock lock(&log_time_lock_);
  DurationClock::time_point now = Now();
  DurationClock::duration logged_delta = DurationClock::duration::min();

  if (last_log_time_ != DurationClock::time_point::min()) {
    // Using initialized logged_delta
    logged_delta = now - last_log_time_;
  }

  last_log_time_ = now;
  return logged_delta;
}

void LayerData::LogEventOnly(std::string_view event_type,
                             std::string_view extra_content) const {
  if (event_log_) {
    const std::string prefix = MakeEventLogPrefix(event_type);
    WriteLnAndFlush(event_log_, extra_content.empty()
                                    ? prefix
                                    : CsvCat(prefix, extra_content));
  }
}

std::string LayerData::ShaderHashToString(uint64_t hash) {
  return absl::StrFormat("%#x", hash);
}

std::string LayerData::PipelineHashToString(const HashVector& pipeline) const {
  return absl::StrCat("[",
                      absl::StrJoin(pipeline, ",",
                                    [](std::string* out, uint64_t hash) {
                                      return absl::StrAppend(
                                          out, ShaderHashToString(hash));
                                    }),
                      "]");
}

VkResult LayerData::CreateInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkInstance* instance,
    std::function<VkLayerInstanceDispatchTable(PFN_vkGetInstanceProcAddr)>
        get_dispatch_table) {
  auto* instance_create_info = FindInstanceCreateInfo(create_info);
  if (instance_create_info == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  auto get_proc_addr =
      instance_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

  // Create the instance by calling the next layer's vkCreateInstance.
  instance_create_info->u.pLayerInfo =
      instance_create_info->u.pLayerInfo->pNext;
  auto create_function = reinterpret_cast<PFN_vkCreateInstance>(
      get_proc_addr(VK_NULL_HANDLE, "vkCreateInstance"));
  VkResult res = create_function(create_info, allocator, instance);
  if (res != VK_SUCCESS) {
    return res;
  }

  // Build dispatch table for the instance functions we need to call.
  VkLayerInstanceDispatchTable dispatch_table =
      get_dispatch_table(get_proc_addr);

  // Add the dispatch table to the dispatch map.
  if (!AddInstance(*instance, dispatch_table)) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  return VK_SUCCESS;
}

VkResult LayerData::CreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDevice* device,
    std::function<VkLayerDispatchTable(PFN_vkGetDeviceProcAddr)>
        get_dispatch_table) {
  assert(create_info != nullptr);

  auto* device_create_info = FindDeviceCreateInfo(create_info);
  if (device_create_info == nullptr) {
    // No loader device create info.
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  assert(device_create_info->u.pLayerInfo);

  PFN_vkGetInstanceProcAddr get_instance_proc_addr =
      device_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr =
      device_create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  VkInstance instance = GetInstance(InstanceKey(physical_device));
  assert(instance);

  // Create the device after removing the current layer.
  device_create_info->u.pLayerInfo = device_create_info->u.pLayerInfo->pNext;
  auto create_function = reinterpret_cast<PFN_vkCreateDevice>(
      get_instance_proc_addr(instance, "vkCreateDevice"));
  assert(create_function);
  VkResult result =
      create_function(physical_device, create_info, allocator, device);

  if (result != VK_SUCCESS) {
    return result;
  }

  // Build dispatch table for the device functions we need to call.
  VkLayerDispatchTable dispatch_table =
      get_dispatch_table(get_device_proc_addr);

  // Add the dispatch_table to the dispatch map.
  if (!AddDevice(*device, dispatch_table)) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  return VK_SUCCESS;
}

LayerData::ShaderModuleCreateResult LayerData::CreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkShaderModule* shader_module) {
  auto next_proc =
      GetNextDeviceProcAddr(device, &VkLayerDispatchTable::CreateShaderModule);
  DurationClock::time_point start = Now();
  VkResult result = next_proc(device, create_info, allocator, shader_module);
  DurationClock::time_point end = Now();
  uint64_t hash =
      HashShader(*shader_module, create_info->pCode, create_info->codeSize);
  return {result, hash, start, end};
}

void LayerData::DestroyShaderModule(VkDevice device,
                                    VkShaderModule shader_module,
                                    const VkAllocationCallbacks* allocator) {
  auto next_proc =
      GetNextDeviceProcAddr(device, &VkLayerDispatchTable::DestroyShaderModule);
  EraseShader(shader_module);
  next_proc(device, shader_module, allocator);
}
}  // namespace performancelayers
