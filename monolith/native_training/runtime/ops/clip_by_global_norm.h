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

#ifndef MONOLITH_NATIVE_TRAINING_RUNTIME_OPS_CLIP_BY_GLOBAL_NORM
#define MONOLITH_NATIVE_TRAINING_RUNTIME_OPS_CLIP_BY_GLOBAL_NORM

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/util/work_sharder.h"

namespace tensorflow {
namespace monolith {

template <typename Device>
struct ClipByGlobalNormImpl {
  static void Compute(OpKernelContext* context, float scale);
};

template <typename Device>
class ClipByGlobalNorm : public OpKernel {
 public:
  explicit ClipByGlobalNorm(OpKernelConstruction* context)
      : OpKernel(context) {}

  void Compute(OpKernelContext* context) override {
    int num_inputs = context->num_inputs() - 2;
    float global_norm = context->input(num_inputs).scalar<float>()();
    float clip_norm = context->input(num_inputs + 1).scalar<float>()();
    if (global_norm > clip_norm) {
      ClipByGlobalNormImpl<Device>::Compute(context, clip_norm / global_norm);
    } else {
      // If no clip, output as input.
      for (int i = 0; i < num_inputs; ++i) {
        context->set_output(i, context->input(i));
      }
    }
  }
};

}  // namespace monolith
}  // namespace tensorflow

#endif  // MONOLITH_NATIVE_TRAINING_RUNTIME_OPS_CLIP_BY_GLOBAL_NORM
