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

#ifndef MONOLITH_MONOLITH_NATIVE_TRAINING_RUNTIME_HASH_TABLE_RETRIEVER_FAKE_QUANT_RETRIEVER_H_
#define MONOLITH_MONOLITH_NATIVE_TRAINING_RUNTIME_HASH_TABLE_RETRIEVER_FAKE_QUANT_RETRIEVER_H_

#include <memory>

#include "monolith/native_training/runtime/hash_table/compressor/fake_quantizer.h"
#include "monolith/native_training/runtime/hash_table/retriever/retriever_interface.h"

namespace monolith {
namespace hash_table {

std::unique_ptr<RetrieverInterface> NewFakeQuantRetriever(int dim_size, const FakeQuantizer& fake_quantizer);

}  // namespace hash_table
}  // namespace monolith

#endif  // MONOLITH_MONOLITH_NATIVE_TRAINING_RUNTIME_HASH_TABLE_RETRIEVER_FAKE_QUANT_RETRIEVER_H_
