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

#include "monolith/native_training/runtime/hash_table/retriever/fake_quant_retriever.h"

#include <memory>

#include "absl/random/random.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace monolith {
namespace hash_table {
namespace {

using ::testing::ElementsAre;
using ::testing::Le;

TEST(FakeQuantRetriever, Basic) {
  int dim_size = 10;
  float r = 1.0f;
  const float kStep = r / 128;
  FakeQuantizer fake_quantizer(1.0f);
  auto retriever = NewFakeQuantRetriever(dim_size, fake_quantizer);
  std::vector<float> entry(dim_size);
  absl::BitGen bit_gen;
  for (auto& val : entry) {
    val = absl::Uniform<float>(bit_gen, -1.f, 1.f);
  }

  std::vector<float> num(dim_size, 0);
  retriever->Retrieve(entry.data(), absl::MakeSpan(num));
  for (int i = 0; i < dim_size; ++i) {
    EXPECT_THAT(std::abs(entry[i] - num[i]), Le(kStep));
  }
}

}  // namespace
}  // namespace hash_table
}  // namespace monolith
