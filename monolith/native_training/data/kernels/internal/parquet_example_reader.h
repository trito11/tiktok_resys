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

#ifndef PARQUET_EXAMPLE_READER_H_
#define PARQUET_EXAMPLE_READER_H_

#include <regex>
#include <string>
#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "idl/matrix/proto/example.pb.h"
#include "monolith/native_training/data/kernels/internal/arrow_random_access_file.h"
#include "monolith/native_training/data/kernels/internal/parquet_column_buffer.h"
#include "monolith/native_training/data/kernels/internal/sized_random_access_file.h"
#include "parquet/api/reader.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/platform/stacktrace.h"
#include "tensorflow/core/profiler/lib/traceme.h"

namespace tensorflow {
namespace data {

using idl::matrix::proto::LineId;
using monolith::io::proto::BytesList;
using monolith::io::proto::DoubleList;
using monolith::io::proto::Example;
using monolith::io::proto::ExampleBatch;
using monolith::io::proto::Feature;
using monolith::io::proto::FidList;
using monolith::io::proto::FloatList;
using monolith::io::proto::Int64List;
using monolith::io::proto::NamedFeature;
using monolith::io::proto::NamedFeatureList;

enum ParsedDataType { INT = 1, FIDV1 = 2, FIDV2 = 3, FLOAT = 4, BYTES = 5 };

class ParquetExampleReader {
 public:
  explicit ParquetExampleReader(Env* env) : env_(env) {}

  virtual ~ParquetExampleReader() {}

  Status Init(std::string file_name,
              const std::vector<std::string>& selected_col_names,
              const std::vector<std::string>& selected_col_types) {
    // open paruqet file, and hold handler
    file_.reset(new SizedRandomAccessFile(env_, file_name, nullptr, 0));
    TF_RETURN_IF_ERROR(file_->GetFileSize(&file_size_));
    parquet_file_.reset(new ArrowRandomAccessFile(file_.get(), file_size_));
    parquet_reader_ =
        parquet::ParquetFileReader::Open(std::move(parquet_file_));
    parquet_metadata_ = parquet_reader_->metadata();

    // register columns names
    columns_.clear();
    for (int i = 0; i < parquet_metadata_->num_columns(); i++) {
      const std::string& full_col_name =
          parquet_metadata_->schema()->Column(i)->path().get()->ToDotString();
      std::vector<absl::string_view> split_result =
          absl::StrSplit(full_col_name, ".");
      if (split_result.empty()) {
        LOG(WARNING) << "Split column full name " << full_col_name
                     << ", get empty result, will skip this column.";
        continue;
      }
      std::string col_name = {split_result[0].data(), split_result[0].size()};
      columns_.push_back(col_name);
      columns_index_map_[col_name] = i;
      col_pure_name_map_[i] = col_name;
    }
    // LOG(INFO) << "parquet file schema: ";
    // for (int i = 0; i < parquet_metadata_->num_columns(); i++) {
    //   parquet::Type::type col_type =
    //       parquet_metadata_->schema()->Column(i)->physical_type();
    //   LOG(INFO) << "column " << i << " : " << columns_[i] << " : "
    //             << ColTypeToString(col_type);
    // }
    // LOG(INFO) << "end of schema";
    LOG(INFO) << "parquet file columns: " << parquet_metadata_->num_columns();
    LOG(INFO) << "parquet file rows: " << parquet_metadata_->num_rows();

    // select column, and check col type
    selected_col_ids_.clear();
    selected_col_feature_type_.clear();
    TF_RETURN_IF_ERROR(SetSelectedCols(selected_col_names, selected_col_types));

    // init global iter_ and row_group related variables
    iter_ = 0;
    row_group_offset_ = -1;
    row_group_id_ = -1;
    row_group_reader_.reset();
    TF_RETURN_IF_ERROR(NextRowGroup());

    // init line_id descriptor
    descriptor_ = ::idl::matrix::proto::LineId::GetDescriptor();
    reflection_ = ::idl::matrix::proto::LineId::GetReflection();
    for (size_t i = 0; i < selected_col_ids_.size(); i++) {
      int64_t col_id = selected_col_ids_[i];
      std::string col_name = col_pure_name_map_[col_id];
      const google::protobuf::FieldDescriptor* field_descriptor =
          GetLineIdFieldByName(col_name);
      line_id_discriptor_map_[col_id] = field_descriptor;
    }

    LOG(INFO) << "Init of ParquetReader Success. file_name = " << file_name;
    return Status::OK();
  }

  static const char* ColTypeToString(parquet::Type::type type) {
    switch (type) {
      case parquet::Type::BOOLEAN:
        return "BOOLEAN";
      case parquet::Type::INT32:
        return "INT32";
      case parquet::Type::INT64:
        return "INT64";
      case parquet::Type::FLOAT:
        return "FLOAT";
      case parquet::Type::DOUBLE:
        return "DOUBLE";
      case parquet::Type::BYTE_ARRAY:
        return "BYTE_ARRAY";
      case parquet::Type::FIXED_LEN_BYTE_ARRAY:
        return "FIXED_LEN_BYTE_ARRAY";
      default:
        return "UNKNOWN";
    }
  }

  Status SetSelectedCols(const std::vector<std::string>& selected_col_names,
                         const std::vector<std::string>& selected_col_types) {
    // check size equal
    if (selected_col_names.size() != selected_col_types.size()) {
      return errors::InvalidArgument(
          "list selected_col_names should have the same size as list "
          "selected_col_types");
    }

    // check column names valid, and not duplicated
    std::unordered_set<uint64_t> selected_col_id_set;
    for (const std::string& col_name : selected_col_names) {
      auto it = columns_index_map_.find(col_name);
      if (it == columns_index_map_.end()) {
        return errors::InvalidArgument("column name: ", col_name,
                                       " not in paruquet schema");
      }
      selected_col_ids_.push_back(it->second);
      selected_col_id_set.insert(it->second);
    }
    if (selected_col_ids_.size() != selected_col_id_set.size()) {
      return errors::InvalidArgument(
          "selected_col_names have duplicate columns");
    }

    // check seleced_col_types if vaild, and fill enum values in
    // selected_col_feature_type_
    for (uint64_t i = 0; i < selected_col_ids_.size(); i++) {
      uint64_t col_id = selected_col_ids_[i];
      const std::string& feature_type = selected_col_types[i];
      const std::string& col_name = selected_col_names[i];
      parquet::Type::type col_type =
          parquet_metadata_->schema()->Column(col_id)->physical_type();
      switch (col_type) {
        case parquet::Type::INT32:
          if (feature_type == "int") {
            selected_col_feature_type_.push_back(ParsedDataType::INT);
          } else {
            return errors::InvalidArgument(
                "invalid selected_col_types, col_name = ", col_name,
                ", feature type should be int");
          }
          break;
        case parquet::Type::INT64:
          if (feature_type == "int") {
            selected_col_feature_type_.push_back(ParsedDataType::INT);
          } else if (feature_type == "fid_v1") {
            selected_col_feature_type_.push_back(ParsedDataType::FIDV1);
          } else if (feature_type == "fid_v2") {
            selected_col_feature_type_.push_back(ParsedDataType::FIDV2);
          } else {
            return errors::InvalidArgument(
                "invalid selected_col_types, col_name = ", col_name,
                ", feature type should in [int, fid_v1, fid_v2]");
          }
          break;
        case parquet::Type::FLOAT:
        case parquet::Type::DOUBLE:
          if (feature_type == "float") {
            selected_col_feature_type_.push_back(ParsedDataType::FLOAT);
          } else {
            return errors::InvalidArgument(
                "invalid selected_col_types, col_name = ", col_name,
                ", feature type should be float");
          }
          break;
        case parquet::Type::BYTE_ARRAY:
          if (feature_type == "bytes") {
            selected_col_feature_type_.push_back(ParsedDataType::BYTES);
          } else {
            return errors::InvalidArgument(
                "invalid selected_col_types, col_name = ", col_name,
                ", feature type should be bytes");
          }
          break;
        default:
          return errors::InvalidArgument(
              "invalid column parquet type col_name = ", col_name,
              "parquet type is", ColTypeToString(col_type));
      }
    }
    return Status::OK();
  }

  const google::protobuf::FieldDescriptor* GetLineIdFieldByName(
      std::string name) {
    static std::regex reg("__[A-Z_]+__");
    bool is_match = std::regex_match(name, reg);
    if (!is_match) {
      return nullptr;
    }
    std::string subname = name.substr(2, name.length() - 4);
    std::string lower_subname = absl::AsciiStrToLower(subname);
    return descriptor_->FindFieldByName(lower_subname);
  }

  Status GetNextExample(Example& example) {
    if (IsEOF()) {
      return errors::OutOfRange("GetNextExample out of range, iter = ", iter_);
    }
    while (iter_ >=
           row_group_offset_ + row_group_reader_->metadata()->num_rows()) {
      TF_RETURN_IF_ERROR(NextRowGroup());
    }

    for (size_t i = 0; i < selected_col_ids_.size(); i++) {
      int64_t col_id = selected_col_ids_[i];
      parquet::Type::type col_type =
          parquet_metadata_->schema()->Column(col_id)->physical_type();
      std::string col_name = col_pure_name_map_[col_id];

      // if column is __LABEL__
      if (col_name == "__LABEL__") {
        if (col_type == parquet::Type::INT32) {
          TF_RETURN_IF_ERROR(FillLabel<parquet::Int32Type>(i, example));
        } else if (col_type == parquet::Type::INT64) {
          TF_RETURN_IF_ERROR(FillLabel<parquet::Int64Type>(i, example));
        } else if (col_type == parquet::Type::FLOAT) {
          TF_RETURN_IF_ERROR(FillLabel<parquet::FloatType>(i, example));
        } else if (col_type == parquet::Type::DOUBLE) {
          TF_RETURN_IF_ERROR(FillLabel<parquet::DoubleType>(i, example));
        } else {
          LOG(FATAL)
              << "In parquet: __LABEL__ column has wrong type, pls check";
        }
        continue;
      }

      // if column in line_id
      auto it = line_id_discriptor_map_.find(col_id);
      const google::protobuf::FieldDescriptor* line_field =
          (it != line_id_discriptor_map_.end()) ? it->second : nullptr;
      if (line_field != nullptr) {
        if (line_field->is_repeated()) {
          // TODO(libo.bob): will support it later
          LOG(FATAL) << "Not support repeated line_id field now.";
        }
        switch (line_field->cpp_type()) {
          case google::protobuf::FieldDescriptor::CppType::CPPTYPE_INT32: {
            CHECK(col_type == parquet::Type::INT32)
                << "column: " << col_name
                << " should have the same type as line_id field";
            reflection_->SetInt32(example.mutable_line_id(), line_field,
                                  GetSingleValue<parquet::Int32Type>(i));
            break;
          }
          case google::protobuf::FieldDescriptor::CppType::CPPTYPE_INT64: {
            CHECK(col_type == parquet::Type::INT64)
                << "column: " << col_name
                << " should have the same type as line_id field";
            reflection_->SetInt64(example.mutable_line_id(), line_field,
                                  GetSingleValue<parquet::Int64Type>(i));
            break;
          }
          case google::protobuf::FieldDescriptor::CppType::CPPTYPE_UINT32: {
            CHECK(col_type == parquet::Type::INT32)
                << "column: " << col_name
                << " should have the same type as line_id field";
            reflection_->SetUInt32(example.mutable_line_id(), line_field,
                                   GetSingleValue<parquet::Int32Type>(i));
            break;
          }
          case google::protobuf::FieldDescriptor::CppType::CPPTYPE_UINT64: {
            CHECK(col_type == parquet::Type::INT64)
                << "column: " << col_name
                << " should have the same type as line_id field";
            reflection_->SetUInt64(example.mutable_line_id(), line_field,
                                   GetSingleValue<parquet::Int64Type>(i));
            break;
          }
          case google::protobuf::FieldDescriptor::CppType::CPPTYPE_FLOAT: {
            CHECK(col_type == parquet::Type::FLOAT)
                << "column: " << col_name
                << " should have the same type as line_id field";
            reflection_->SetFloat(example.mutable_line_id(), line_field,
                                  GetSingleValue<parquet::FloatType>(i));
            break;
          }
          case google::protobuf::FieldDescriptor::CppType::CPPTYPE_DOUBLE: {
            CHECK(col_type == parquet::Type::DOUBLE)
                << "column: " << col_name
                << " should have the same type as line_id field";
            reflection_->SetDouble(example.mutable_line_id(), line_field,
                                   GetSingleValue<parquet::DoubleType>(i));
            break;
          }
          case google::protobuf::FieldDescriptor::CppType::CPPTYPE_STRING: {
            CHECK(col_type == parquet::Type::BYTE_ARRAY)
                << "column: " << col_name
                << " should have the same type as line_id field";
            std::string value =
                ByteArrayToString(GetSingleValue<parquet::ByteArrayType>(i));
            reflection_->SetString(example.mutable_line_id(), line_field,
                                   value);
            break;
          }
          default:
            LOG(FATAL) << "not support line_id type for column " << col_name;
        }
        continue;
      }

      NamedFeature* named_feature = example.add_named_feature();
      named_feature->set_id(col_id + 10000);
      named_feature->set_name(col_name);
      Feature* feature = named_feature->mutable_feature();
      switch (col_type) {
        case parquet::Type::INT32: {
          TF_RETURN_IF_ERROR(FillValueList<parquet::Int32Type, Int64List>(
              i, feature->mutable_int64_list()));
          break;
        }
        case parquet::Type::INT64: {
          if (selected_col_feature_type_[i] == ParsedDataType::INT) {
            TF_RETURN_IF_ERROR(FillValueList<parquet::Int64Type, Int64List>(
                i, feature->mutable_int64_list()));
          } else if (selected_col_feature_type_[i] == ParsedDataType::FIDV1) {
            TF_RETURN_IF_ERROR(FillValueList<parquet::Int64Type, FidList>(
                i, feature->mutable_fid_v1_list()));
          } else {
            TF_RETURN_IF_ERROR(FillValueList<parquet::Int64Type, FidList>(
                i, feature->mutable_fid_v2_list()));
          }
          break;
        }
        case parquet::Type::FLOAT: {
          TF_RETURN_IF_ERROR(FillValueList<parquet::FloatType, FloatList>(
              i, feature->mutable_float_list()));
          break;
        }
        case parquet::Type::DOUBLE: {
          TF_RETURN_IF_ERROR(FillValueList<parquet::DoubleType, FloatList>(
              i, feature->mutable_float_list()));
          break;
        }
        case parquet::Type::BYTE_ARRAY: {
          TypedColumnBuffer<parquet::ByteArrayType>* typed_col_buf =
              dynamic_cast<TypedColumnBuffer<parquet::ByteArrayType>*>(
                  col_buffers_[i].get());
          std::vector<parquet::ByteArray> values;
          TF_RETURN_IF_ERROR(typed_col_buf->GetNextValues(values));
          BytesList* bytes_list = feature->mutable_bytes_list();
          for (const parquet::ByteArray& value : values) {
            bytes_list->add_value(ByteArrayToString(value));
          }
          break;
        }
        default:
          return errors::InvalidArgument("not support column type");
      }
    }
    iter_++;
    return Status::OK();
  }

  Status GetNextExampleBatch(ExampleBatch& example_batch, int64_t batch_size) {
    if (IsEOF()) {
      return errors::OutOfRange("GetNextExampleBatch out of range, iter = ",
                                iter_);
    }
    // cread namedfeaturelist(s)
    {
      profiler::TraceMe activity(
          []() { return "ParquetDataset::CreateNamedFeatureLists"; });
      for (size_t i = 0; i < selected_col_ids_.size(); i++) {
        int64_t col_id = selected_col_ids_[i];
        const std::string& col_name = col_pure_name_map_[col_id];
        NamedFeatureList* named_feature_list =
            example_batch.add_named_feature_list();
        named_feature_list->set_id(col_id);
        named_feature_list->set_name(col_name);
      }
    }

    // calculate batch_size
    int64_t rows_to_read_left =
        iter_ + batch_size >= parquet_metadata_->num_rows()
            ? parquet_metadata_->num_rows() - iter_
            : batch_size;
    example_batch.set_batch_size(rows_to_read_left);

    // read features for each column
    while (rows_to_read_left > 0) {
      // if need to go to next row group
      while (iter_ >=
             row_group_offset_ + row_group_reader_->metadata()->num_rows()) {
        TF_RETURN_IF_ERROR(NextRowGroup());
      }
      // calculate max rows can read in current row group
      int64_t rows_in_row_group =
          iter_ + rows_to_read_left >=
                  row_group_offset_ + row_group_reader_->metadata()->num_rows()
              ? row_group_offset_ + row_group_reader_->metadata()->num_rows() -
                    iter_
              : rows_to_read_left;
      rows_to_read_left -= rows_in_row_group;
      // read from current row group
      for (size_t i = 0; i < selected_col_ids_.size(); i++) {
        profiler::TraceMe activity(
            []() { return "ParquetDataset::ReadOneColumnWithBatchSize"; });
        int64_t col_id = selected_col_ids_[i];
        parquet::Type::type col_type =
            parquet_metadata_->schema()->Column(col_id)->physical_type();
        NamedFeatureList* named_feature_list =
            example_batch.mutable_named_feature_list(i);
        for (int64_t ft = 0; ft < rows_in_row_group; ft++) {
          Feature* feature = named_feature_list->add_feature();
          switch (col_type) {
            case parquet::Type::INT32: {
              TF_RETURN_IF_ERROR(FillValueList<parquet::Int32Type, Int64List>(
                  i, feature->mutable_int64_list()));
              break;
            }
            case parquet::Type::INT64: {
              if (selected_col_feature_type_[i] == ParsedDataType::INT) {
                TF_RETURN_IF_ERROR(FillValueList<parquet::Int64Type, Int64List>(
                    i, feature->mutable_int64_list()));
              } else if (selected_col_feature_type_[i] ==
                         ParsedDataType::FIDV1) {
                TF_RETURN_IF_ERROR(FillValueList<parquet::Int64Type, FidList>(
                    i, feature->mutable_fid_v1_list()));
              } else {
                TF_RETURN_IF_ERROR(FillValueList<parquet::Int64Type, FidList>(
                    i, feature->mutable_fid_v2_list()));
              }
              break;
            }
            case parquet::Type::FLOAT: {
              TF_RETURN_IF_ERROR(FillValueList<parquet::FloatType, FloatList>(
                  i, feature->mutable_float_list()));
              break;
            }
            case parquet::Type::DOUBLE: {
              TF_RETURN_IF_ERROR(FillValueList<parquet::DoubleType, FloatList>(
                  i, feature->mutable_float_list()));
              break;
            }
            case parquet::Type::BYTE_ARRAY: {
              TypedColumnBuffer<parquet::ByteArrayType>* typed_col_buf =
                  dynamic_cast<TypedColumnBuffer<parquet::ByteArrayType>*>(
                      col_buffers_[i].get());
              std::vector<parquet::ByteArrayType::c_type> values;
              TF_RETURN_IF_ERROR(typed_col_buf->GetNextValues(values));
              BytesList* bytes_list = feature->mutable_bytes_list();
              for (const parquet::ByteArrayType::c_type& value : values) {
                bytes_list->add_value(ByteArrayToString(value));
              }
              break;
            }
            default:
              return errors::InvalidArgument("not support column type");
          }
        }
      }
      iter_ += rows_in_row_group;
    }

    return Status::OK();
  }

  template <typename PARQUET_TYPE, typename PB_TLIST>
  Status FillValueList(int64_t col_buffer_id, PB_TLIST* value_list) {
    TypedColumnBuffer<PARQUET_TYPE>* typed_col_buf =
        dynamic_cast<TypedColumnBuffer<PARQUET_TYPE>*>(
            col_buffers_[col_buffer_id].get());
    std::vector<typename PARQUET_TYPE::c_type> values;
    Status status = typed_col_buf->GetNextValues(values);
    if (!status.ok()) {
      std::string stack_trace = CurrentStackTrace();
      LOG(INFO) << stack_trace;
      return status;
    }
    for (const typename PARQUET_TYPE::c_type& value : values) {
      value_list->add_value(value);
    }
    return Status::OK();
  }

  template <typename PARQUET_TYPE>
  Status FillLabel(int64_t col_buffer_id, Example& example) {
    TypedColumnBuffer<PARQUET_TYPE>* typed_col_buf =
        dynamic_cast<TypedColumnBuffer<PARQUET_TYPE>*>(
            col_buffers_[col_buffer_id].get());
    std::vector<typename PARQUET_TYPE::c_type> values;
    Status status = typed_col_buf->GetNextValues(values);
    if (!status.ok()) {
      std::string stack_trace = CurrentStackTrace();
      LOG(INFO) << stack_trace;
      return status;
    }
    for (const typename PARQUET_TYPE::c_type& value : values) {
      example.mutable_label()->Add(static_cast<float>(value));
    }
    return Status::OK();
  }

  template <typename PARQUET_TYPE>
  typename PARQUET_TYPE::c_type GetSingleValue(int64_t col_buffer_id) {
    TypedColumnBuffer<PARQUET_TYPE>* typed_col_buf =
        dynamic_cast<TypedColumnBuffer<PARQUET_TYPE>*>(
            col_buffers_[col_buffer_id].get());
    std::vector<typename PARQUET_TYPE::c_type> values;
    Status status = typed_col_buf->GetNextValues(values);
    if (!status.ok()) {
      std::string stack_trace = CurrentStackTrace();
      LOG(FATAL) << stack_trace;
    }
    if (values.size() != 1) {
      LOG(FATAL) << "Parquet column id = " << col_buffer_id
                 << ", should have single value for one row, but got "
                 << values.size();
    }
    return values[0];
  }

  Status NextRowGroup() {
    profiler::TraceMe activity([]() { return "ParquetDataset::NextRowGroup"; });
    if (row_group_id_ + 1 >= parquet_metadata_->num_row_groups()) {
      return errors::OutOfRange("row group out of range");
    }
    if (!row_group_reader_) {
      // first initialize
      row_group_reader_ = parquet_reader_->RowGroup(0);
      row_group_id_ = 0;
      row_group_offset_ = 0;
    } else {
      row_group_offset_ =
          row_group_offset_ + row_group_reader_->metadata()->num_rows();
      row_group_id_++;
      row_group_reader_ = parquet_reader_->RowGroup(row_group_id_);
    }
    // update col_buffers
    col_buffers_.clear();
    for (uint64_t col_id : selected_col_ids_) {
      std::shared_ptr<parquet::ColumnReader> column_reader =
          row_group_reader_->Column(col_id);
      switch (parquet_metadata_->schema()->Column(col_id)->physical_type()) {
        case parquet::Type::INT32:
          col_buffers_.emplace_back(
              new TypedColumnBuffer<parquet::Int32Type>(column_reader));
          break;
        case parquet::Type::INT64:
          col_buffers_.emplace_back(
              new TypedColumnBuffer<parquet::Int64Type>(column_reader));
          break;
        case parquet::Type::FLOAT:
          col_buffers_.emplace_back(
              new TypedColumnBuffer<parquet::FloatType>(column_reader));
          break;
        case parquet::Type::DOUBLE:
          col_buffers_.emplace_back(
              new TypedColumnBuffer<parquet::DoubleType>(column_reader));
          break;
        case parquet::Type::BYTE_ARRAY:
          col_buffers_.emplace_back(
              new TypedColumnBuffer<parquet::ByteArrayType>(column_reader));
          break;
        default:
          return errors::InvalidArgument("not support column type");
      }
    }
    return Status::OK();
  }

  bool IsEOF() {
    // LOG(INFO) << "iter_ = " << iter_;
    return iter_ >= parquet_metadata_->num_rows();
  }

 private:
  Env* env_;
  std::unique_ptr<SizedRandomAccessFile> file_;
  uint64 file_size_;
  std::unique_ptr<ArrowRandomAccessFile> parquet_file_;

  std::string file_name_;
  std::shared_ptr<::parquet::ParquetFileReader> parquet_reader_;

  std::shared_ptr<::parquet::FileMetaData> parquet_metadata_;
  std::vector<std::string> columns_;
  std::unordered_map<std::string, int64> columns_index_map_;
  std::unordered_map<int64_t, std::string> col_pure_name_map_;

  std::vector<uint64_t> selected_col_ids_;
  std::vector<ParsedDataType> selected_col_feature_type_;

  // iter_ and row_group variables
  int64_t iter_;
  std::shared_ptr<parquet::RowGroupReader> row_group_reader_;
  int64_t row_group_id_;
  int64_t row_group_offset_;

  std::vector<std::shared_ptr<ColumnBuffer>> col_buffers_;

  // line_id related
  const google::protobuf::Descriptor* descriptor_;
  const google::protobuf::Reflection* reflection_;
  std::unordered_map<int64_t, const google::protobuf::FieldDescriptor*>
      line_id_discriptor_map_;
};

}  // namespace data
}  // namespace tensorflow

#endif  // PARQUET_EXAMPLE_READER_H_
