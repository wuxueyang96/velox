/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <memory>

/// Contains wrappers for common Cuda objects. Wave does not directly
/// include Cuda headers because of interference with BitUtils.h and
/// SimdUtils.h.
namespace facebook::velox::wave {

struct Device {
  explicit Device(int32_t id) : deviceId(id) {}

  int32_t deviceId;
};

/// Checks that the machine has the right capability and returns a Device
/// struct. If 'preferredId' is given tries to return  a Device on that device
/// id.
Device* getDevice(int32_t preferredId = -1);
/// Binds subsequent Cuda operations of the calling thread to 'device'.
void setDevice(Device* device);

struct StreamImpl;

struct Stream {
  Stream();
  virtual ~Stream();

  /// Waits  until the stream is completed.
  void wait();

  /// Enqueus a prefetch. Prefetches to host if 'device' is nullptr, otherwise
  /// to 'device'.
  void prefetch(Device* device, void* address, size_t size);
  std::unique_ptr<StreamImpl> stream;
};

// Abstract class wrapping device or universal address memory allocation.
class GpuAllocator {
 public:
  virtual ~GpuAllocator() = default;

  // Returns a pointer to at least 'bytes' of universal or device memory,
  // depending on specific allocator. The size can be rounded up. The alignment
  // is to 8 bytes.
  virtual void* allocate(size_t bytes) = 0;

  /// Frees a pointer from allocate(). 'size' must correspond to the size given
  /// to allocate(). A Memory must be freed to the same allocator it came from.
  virtual void free(void* ptr, size_t bytes) = 0;
};

GpuAllocator* getAllocator(Device* device);

} // namespace facebook::velox::wave
