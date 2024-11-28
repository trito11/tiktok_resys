// Copyright 2022 ByteDance and/or its affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MONOLITH_NATIVE_TRAINING_RUNTIME_OPS_GPU_MULTI_HASH_TABLE
#define MONOLITH_NATIVE_TRAINING_RUNTIME_OPS_GPU_MULTI_HASH_TABLE
#ifdef GOOGLE_CUDA
#define EIGEN_USE_GPU
#include "monolith/native_training/runtime/hash_table/GPUcucohash/cuco_multi_table_ops.cuh.h"
#include "monolith/native_training/runtime/ops/multi_hash_table.h"

namespace tensorflow {
namespace monolith_tf {

class GpuMultiHashTable : public MultiHashTable {
 public:
  ::monolith::hash_table::CucoMultiHashTableOp op;
  explicit GpuMultiHashTable(
      absl::string_view shared_name, std::vector<int> slot_occ = {},
      ::monolith::hash_table::GpucucoEmbeddingHashTableConfig config = {},
      cudaStream_t stream = 0)
      : MultiHashTable(shared_name),
        op(std::move(slot_occ), std::move(config), stream) {}
};

}  // namespace monolith_tf
}  // namespace tensorflow
#endif
#endif  // MONOLITH_NATIVE_TRAINING_RUNTIME_OPS_GPU_MULTI_HASH_TABLE
