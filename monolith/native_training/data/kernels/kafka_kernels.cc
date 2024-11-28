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

/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "rdkafkacpp.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/resource_op_kernel.h"

#include "monolith/native_training/data/kernels/feature_name_mapper_tf_bridge.h"
#include "monolith/native_training/data/training_instance/cc/data_reader.h"

namespace tensorflow {
namespace monolith_tf {
namespace {
using Example = ::monolith::io::proto::Example;
using ExampleBatch = ::monolith::io::proto::ExampleBatch;
using Instance = ::parser::proto::Instance;
using ::tensorflow::monolith_tf::BaseStreamReader;
using ::tensorflow::monolith_tf::DataFormatOptions;
using ::tensorflow::monolith_tf::ExampleBatchIterator;
using ::tensorflow::monolith_tf::ExampleToInstance;
using ::tensorflow::monolith_tf::FeatureNameMapper;
using ::tensorflow::monolith_tf::FeatureNameMapperTfBridge;
using ::tensorflow::monolith_tf::FeaturePruningType;
using ::tensorflow::monolith_tf::FileStreamReader;
using ::tensorflow::monolith_tf::InstanceToExample;
using ::tensorflow::monolith_tf::PBIterator;
using ::tensorflow::monolith_tf::StdinStreamReader;

}  // namespace

class KafkaEventCb : public RdKafka::EventCb {
 public:
  KafkaEventCb() : run_(true) {}

  bool run() { return run_; }

  void event_cb(RdKafka::Event& event) {
    switch (event.type()) {
      case RdKafka::Event::EVENT_ERROR:
        LOG(ERROR) << "EVENT_ERROR: "
                   << "(" << RdKafka::err2str(event.err())
                   << "): " << event.str();
        { run_ = !event.fatal(); }
        break;
      case RdKafka::Event::EVENT_STATS:
        LOG(ERROR) << "EVENT_STATS: " << event.str();
        break;
      case RdKafka::Event::EVENT_LOG:
        LOG(ERROR) << "EVENT_LOG: " << event.severity() << "-"
                   << event.fac().c_str() << "-" << event.str().c_str();
        break;
      case RdKafka::Event::EVENT_THROTTLE:
        LOG(ERROR) << "EVENT_THROTTLE: " << event.throttle_time() << "ms by "
                   << event.broker_name() << " id "
                   << static_cast<int>(event.broker_id());
        break;
      default:
        LOG(ERROR) << "EVENT: " << event.type() << " ("
                   << RdKafka::err2str(event.err()) << "): " << event.str();
        break;
    }
  }

 private:
  mutable mutex mu_;
  bool run_ TF_GUARDED_BY(mu_) = true;
};

static int64 partition_count = 0;
static int64 eof_count = 0;
class KafkaRebalanceCb : public RdKafka::RebalanceCb {
 public:
  KafkaRebalanceCb() : run_(true) {}

  bool run() { return run_; }

  void rebalance_cb(RdKafka::KafkaConsumer* consumer, RdKafka::ErrorCode err,
                    std::vector<RdKafka::TopicPartition*>& partitions) {
    LOG(ERROR) << "REBALANCE: " << RdKafka::err2str(err);
    int timeout = 5000;  // milliseconds
    LOG(ERROR) << "Retrieved committed offsets with status code: "
               << consumer->committed(partitions, timeout);

    for (int partition = 0; partition < partitions.size(); partition++) {
      // OFFSET MAPPINGS:
      //
      // RD_KAFKA_OFFSET_BEGINNING      -2
      // RD_KAFKA_OFFSET_END            -1
      // RD_KAFKA_OFFSET_STORED         -1000
      // RD_KAFKA_OFFSET_INVALID        -1001

      LOG(INFO) << "REBALANCE: " << partitions[partition]->topic() << "["
                << partitions[partition]->partition() << "], "
                << "OFFSET: " << partitions[partition]->offset() << " "
                << "ERROR_CODE: " << partitions[partition]->err();
    }
    if (err == RdKafka::ERR__ASSIGN_PARTITIONS) {
      // librdkafka does not actually look up the stored offsets before
      // calling your rebalance callback, the partition offsets are set to
      // RD_KAFKA_OFFSET_INVALID at this point to allow us to change it to use
      // some sort of external offset store. But calling assign() with offset
      // RD_KAFKA_OFFSET_INVALID will cause librdkafka to look up the stored
      // offset on the broker.
      // If there was no stored offset it will fall back to `auto.offset.reset`
      // configuration parameter.

      LOG(INFO) << "REBALANCE: Assigning partitions";
      consumer->assign(partitions);
      partition_count = static_cast<int>(partitions.size());
    } else {
      LOG(INFO) << "REBALANCE: Unassigning partitions";
      consumer->unassign();
      partition_count = 0;
    }
    eof_count = 0;
  }

 private:
  mutable mutex mu_;
  bool run_ TF_GUARDED_BY(mu_) = true;
};

class KafkaGroupReadableResource : public ResourceBase {
 public:
  explicit KafkaGroupReadableResource(Env* env) : env_(env) {}
  virtual ~KafkaGroupReadableResource() {
    if (consumer_.get()) {
      consumer_->unassign();
      consumer_->close();
      consumer_.reset(nullptr);
    }
  }

  virtual Status Init(const std::vector<std::string>& topics,
                      const std::vector<std::string>& metadata,
                      const DataFormatOptions& options,
                      const std::string& input_pb_type,
                      const std::string& output_pb_type) {
    mutex_lock l(mu_);

    std::unique_ptr<RdKafka::Conf> conf(
        RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    std::unique_ptr<RdKafka::Conf> conf_topic(
        RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC));

    string errstr;
    RdKafka::Conf::ConfResult result = RdKafka::Conf::CONF_UNKNOWN;

    // The default kafka topic configurations are set first before
    // setting the global confs
    for (size_t i = 0; i < metadata.size(); i++) {
      if (metadata[i].find("conf.topic.") == 0) {
        std::vector<string> parts = str_util::Split(metadata[i], "=");
        if (parts.size() != 2) {
          return errors::InvalidArgument("invalid topic configuration: ",
                                         metadata[i]);
        }
        result = conf_topic->set(parts[0].substr(11), parts[1], errstr);
        if (result != RdKafka::Conf::CONF_OK) {
          return errors::Internal("failed to do topic configuration:",
                                  metadata[i], "error:", errstr);
        }
        LOG(INFO) << "Kafka configuration: " << metadata[i];
      }
    }
    if ((result = conf->set("default_topic_conf", conf_topic.get(), errstr)) !=
        RdKafka::Conf::CONF_OK) {
      return errors::Internal("failed to set default_topic_conf:", errstr);
    }

    // Once the `default_topic_conf` is set, the global confs can now be set
    // without any risk of being overwritten.
    // Setting the global confs before setting the `default_topic_conf`
    // results in erratic behaviour.
    for (size_t i = 0; i < metadata.size(); i++) {
      if (metadata[i] != "" && metadata[i].find("conf.") == string::npos) {
        std::vector<string> parts = str_util::Split(metadata[i], "=");
        if (parts.size() != 2) {
          return errors::InvalidArgument("invalid topic configuration: ",
                                         metadata[i]);
        }
        if ((result = conf->set(parts[0], parts[1], errstr)) !=
            RdKafka::Conf::CONF_OK) {
          return errors::Internal("failed to do global configuration: ",
                                  metadata[i], "error:", errstr);
        }
        LOG(INFO) << "Kafka configuration: " << metadata[i];
      }
    }

    // default consumer.properties:
    //   bootstrap.servers=localhost:9092
    //   group.id=test-consumer-group

    string bootstrap_servers;
    if ((result = conf->get("bootstrap.servers", bootstrap_servers)) !=
        RdKafka::Conf::CONF_OK) {
      bootstrap_servers = "localhost:9092";
      if ((result = conf->set("bootstrap.servers", bootstrap_servers,
                              errstr)) != RdKafka::Conf::CONF_OK) {
        return errors::Internal("failed to set bootstrap.servers [",
                                bootstrap_servers, "]:", errstr);
      }
    }
    string group_id;
    if ((result = conf->get("group.id", group_id)) != RdKafka::Conf::CONF_OK) {
      group_id = "test-consumer-group";
      if ((result = conf->set("group.id", group_id, errstr)) !=
          RdKafka::Conf::CONF_OK) {
        return errors::Internal("failed to set group.id [", group_id,
                                "]:", errstr);
      }
    }

    // Always set enable.partition.eof=true
    if ((result = conf->set("enable.partition.eof", "true", errstr)) !=
        RdKafka::Conf::CONF_OK) {
      return errors::Internal("Failed to set enable.partition.eof=true :",
                              errstr);
    }

    if ((result = conf->set("event_cb", &kafka_event_cb_, errstr)) !=
        RdKafka::Conf::CONF_OK) {
      return errors::Internal("failed to set event_cb:", errstr);
    }

    if ((result = conf->set("rebalance_cb", &kafka_rebalance_cb_, errstr)) !=
        RdKafka::Conf::CONF_OK) {
      return errors::Internal("failed to set rebalance_cb:", errstr);
    }

    // set max.poll.records configuration
    std::string batch_num_messages;
    if ((result = conf->get("batch.num.messages", batch_num_messages)) !=
        RdKafka::Conf::CONF_OK) {
      batch_num_messages = "1024";
      if ((result = conf->set("batch.num.messages", batch_num_messages,
                              errstr)) != RdKafka::Conf::CONF_OK) {
        return errors::Internal("failed to set batch.num.messages [",
                                batch_num_messages, "]:", errstr);
      }
    }
    sscanf(batch_num_messages.c_str(), "%d", &batch_num_messages_);
    LOG(INFO) << "max num of messages per batch: " << batch_num_messages_;

    LOG(INFO) << "Creating the kafka consumer";
    consumer_.reset(RdKafka::KafkaConsumer::create(conf.get(), errstr));
    if (!consumer_.get()) {
      return errors::Internal("failed to create consumer:", errstr);
    }

    for (int i = 0; i < topics.size(); i++) {
      LOG(INFO) << "Subscribing to the kafka topic: " << topics[i];
    }
    RdKafka::ErrorCode err = consumer_->subscribe(topics);
    if (err != RdKafka::ERR_NO_ERROR) {
      return errors::Internal("failed to subscribe to topics: ",
                              RdKafka::err2str(err));
    }

    if (input_pb_type == "" && output_pb_type == "") {
      version_ = 1;
    } else {
      input_pb_type_ = data_format::StringToDataFormat(input_pb_type);
      output_pb_type_ = data_format::StringToDataFormat(output_pb_type);
      if (input_pb_type_ == data_format::UNKNOW ||
          output_pb_type_ == data_format::UNKNOW) {
        return errors::Internal("input_pb_type or output_pb_type err:",
                                input_pb_type, output_pb_type);
      }
      version_ = 2;
    }

    options_ = options;
    return Status::OK();
  }

  class CurPBIteratorHandler {
   public:
    struct CurOutput : public PBIteratorWithDataFormatTransBaseOutput {
      std::vector<Example> exa_pb_list;
      std::vector<Instance> ins_pb_list;
      std::vector<ExampleBatch> eb_pb_list;
      std::vector<tstring> string_list;
    };

    Status HandleReaderNextStauts(const Status& s, const tstring& result) {
      if (s != Status::OK()) {
        if (s.code() != error::OUT_OF_RANGE) {
          LOG(ERROR) << "pb parse error:" << s;
        }
        return s;
      }
      if (result.size() == 0) {
        LOG(ERROR) << "tstring size can not be 0";
        return errors::FailedPrecondition("tstring size=0");
      }
      return Status::OK();
    }

    template <class TResult>
    Status HandleReaderNextStauts(const Status& s, const TResult& result) {
      if (s != Status::OK()) {
        if (s.code() != error::OUT_OF_RANGE) {
          LOG(ERROR) << "pb parse error:" << s;
        }
        return s;
      }

      if (result.ByteSize() == 0) {
        LOG(ERROR) << "pb struct size can not be 0";
        return errors::FailedPrecondition("pb size=0");
      }

      return Status::OK();
    }

    template <class TResult>
    Status HandleResult(TResult&& result, CurOutput* output) {
      return errors::Unimplemented("not implement");
    }

    Status HandleResult(tstring&& serialized, CurOutput* output) {
      output->string_list.emplace_back(std::move(serialized));
      return Status::OK();
    }

    virtual Status HandleResult(Example&& exa_pb, CurOutput* output) {
      output->exa_pb_list.emplace_back(std::move(exa_pb));
      return Status::OK();
    }

    virtual Status HandleResult(Instance&& ins_pb, CurOutput* output) {
      output->ins_pb_list.emplace_back(std::move(ins_pb));
      return Status::OK();
    }

    virtual Status HandleResult(ExampleBatch&& eb_pb, CurOutput* output) {
      output->eb_pb_list.emplace_back(std::move(eb_pb));
      return Status::OK();
    }
  };

  Status Next(const int64 index, const int64 message_poll_timeout,
              const int64 stream_timeout,
              std::function<Status(const TensorShape& shape, Tensor** message,
                                   Tensor** key, Tensor** continue_fetch)>
                  allocate_func) {
    mutex_lock l(mu_);

    // Initialize necessary variables
    int64 num_messages = 0;
    max_stream_timeout_polls_ = stream_timeout / message_poll_timeout;

    // Allocate memory for message_value and key_value vectors
    std::vector<tstring> message_value, key_value;
    message_value.reserve(batch_num_messages_);
    // key_value.reserve(batch_num_messages_);

    std::unique_ptr<RdKafka::Message> message;
    while (consumer_.get() != nullptr && num_messages < batch_num_messages_) {
      if (!kafka_event_cb_.run()) {
        return errors::Internal(
            "failed to consume messages due to broker issue");
      }
      message.reset(consumer_->consume(message_poll_timeout));
      if (message->err() == RdKafka::ERR_NO_ERROR) {
        // Produce the line as output.
        message_value.emplace_back(tstring(
            static_cast<const char*>(message->payload()), message->len()));
        // key_value.emplace_back(
        //     (message->key() != nullptr) ? tstring(*message->key()) : "");
        num_messages++;
        // Once a message has been successfully retrieved, the
        // `stream_timeout_polls_` is reset to 0. This allows the dataset
        // to wait for the entire `stream_timeout` duration when a data
        // slump occurs in the future.
        stream_timeout_polls_ = 0;
      } else if (message->err() == RdKafka::ERR__TRANSPORT) {
        // Not returning an error here as the consumer will try to re-connect.
        LOG(ERROR) << "Broker transport failure: " << message->errstr();

      } else if (message->err() == RdKafka::ERR__PARTITION_EOF) {
        if (++eof_count == partition_count) {
          LOG(INFO) << "EOF reached for all " << partition_count
                    << " partition(s)";
          break;
        }
      } else if (message->err() == RdKafka::ERR__TIMED_OUT) {
        LOG(ERROR) << message->errstr();
        stream_timeout_polls_++;
        break;
      } else {
        LOG(ERROR) << "ERROR Code " << message->err() << ", errstr is "
                   << message->errstr();
      }
    }

    // Prepare the outputs

    PBIteratorWithDataFormatTrans<CurPBIteratorHandler> cur_iter(
        input_pb_type_, output_pb_type_);
    CurPBIteratorHandler::CurOutput output;
    if (version_ == 1) {
      output.string_list.swap(message_value);
    } else {
      // std::ostringstream imploded;
      // std::copy(message_value.begin(), message_value.end(),
      //           std::ostream_iterator<std::string>(imploded, ""));
      //  std::string msg;
      std::unique_ptr<PBIterator> reader;
      for (auto& mesg : message_value) {
        auto stream_reader =
            std::make_unique<StringStreamReader<tstring> >(options_, mesg);
        if (input_pb_type_ == data_format::INSTANCE ||
            input_pb_type_ == data_format::EXAMPLE) {
          reader = absl::make_unique<PBIterator>(
              std::move(stream_reader),
              FeaturePruningType::PRUNING_RAW_FEATURE);
        } else {
          reader = absl::make_unique<ExampleBatchIterator>(
              std::move(stream_reader), FeaturePruningType::PRUNING_RAW_FEATURE,
              &fake_mapper_);
        }

        uint64 offset_ = 0;
        while (true) {
          Status s = cur_iter.GetNext(reader.get(), &output, &offset_);
          if (!s.ok()) break;
          offset_ = reader->GetOffset();
        }
      }
    }
    size_t all_size = 0;
    if (output_pb_type_ == data_format::EXAMPLE) {
      all_size = output.exa_pb_list.size();
    } else if (output_pb_type_ == data_format::EXAMPLEBATCH) {
      all_size = output.eb_pb_list.size();
    } else if (output_pb_type_ == data_format::INSTANCE) {
      all_size = output.ins_pb_list.size();
    } else {
      all_size = output.string_list.size();
    }
    if (all_size < message_value.size()) {
      LOG(ERROR) << "get not enough pb:" << all_size << ","
                 << message_value.size();
    }
    TensorShape shape({static_cast<int64>(all_size)});
    Tensor* message_tensor;
    Tensor* key_tensor;
    Tensor* continue_fetch_tensor;
    TF_RETURN_IF_ERROR(allocate_func(shape, &message_tensor, &key_tensor,
                                     &continue_fetch_tensor));

    for (int i = 0; i < all_size; ++i) {
      if (output_pb_type_ == data_format::EXAMPLE) {
        message_tensor->flat<Variant>()(i) = std::move(output.exa_pb_list[i]);
      } else if (output_pb_type_ == data_format::INSTANCE) {
        message_tensor->flat<Variant>()(i) = std::move(output.ins_pb_list[i]);
      } else if (output_pb_type_ == data_format::EXAMPLEBATCH) {
        message_tensor->flat<Variant>()(i) = std::move(output.eb_pb_list[i]);
      } else {
        message_tensor->flat<tstring>()(i) = std::move(output.string_list[i]);
      }
    }
    if (stream_timeout_polls_ < max_stream_timeout_polls_) {
      continue_fetch_tensor->scalar<int64>()() = 1;
    } else {
      continue_fetch_tensor->scalar<int64>()() = 0;
    }
    LOG_EVERY_N_SEC(INFO, 60)
        << "consumer pb:" << all_size << "," << message_value.size();
    return Status::OK();
  }

  string DebugString() const override { return "KafkaBaseResource"; }

  mutable mutex mu_;
  Env* env_ TF_GUARDED_BY(mu_);
  std::unique_ptr<RdKafka::KafkaConsumer> consumer_ TF_GUARDED_BY(mu_);
  KafkaEventCb kafka_event_cb_ = KafkaEventCb();
  KafkaRebalanceCb kafka_rebalance_cb_ = KafkaRebalanceCb();
  int64 max_stream_timeout_polls_ = -1;
  int64 stream_timeout_polls_ = -1;
  int batch_num_messages_ = 1024;
  // std::unique_ptr<PBIterator> reader_;
  // std::unique_ptr<BaseStreamReader> stream_reader_;
  data_format::DataFormat output_pb_type_;
  data_format::DataFormat input_pb_type_;
  DataFormatOptions options_;
  FeatureNameMapper fake_mapper_;
  int version_ = 1;
};

class KafkaGroupReadableInitOp
    : public ResourceOpKernel<KafkaGroupReadableResource> {
 public:
  explicit KafkaGroupReadableInitOp(OpKernelConstruction* context)
      : ResourceOpKernel<KafkaGroupReadableResource>(context) {
    env_ = context->env();

    OP_REQUIRES_OK(context, context->GetAttr("lagrangex_header",
                                             &options_.lagrangex_header));
    OP_REQUIRES_OK(context, context->GetAttr("kafka_dump_prefix",
                                             &options_.kafka_dump_prefix));
    OP_REQUIRES_OK(context,
                   context->GetAttr("has_sort_id", &options_.has_sort_id));
    OP_REQUIRES_OK(context,
                   context->GetAttr("kafka_dump", &options_.kafka_dump));
    std::string input_pb_type, output_pb_type;
    OP_REQUIRES_OK(context, context->GetAttr("input_pb_type", &input_pb_type_));
    OP_REQUIRES_OK(context,
                   context->GetAttr("output_pb_type", &output_pb_type_));
  }

 private:
  void Compute(OpKernelContext* context) override {
    ResourceOpKernel<KafkaGroupReadableResource>::Compute(context);

    const Tensor* topics_tensor;
    OP_REQUIRES_OK(context, context->input("topics", &topics_tensor));
    std::vector<string> topics;
    for (int64 i = 0; i < topics_tensor->NumElements(); i++) {
      topics.push_back(topics_tensor->flat<tstring>()(i));
    }

    const Tensor* metadata_tensor;
    OP_REQUIRES_OK(context, context->input("metadata", &metadata_tensor));
    std::vector<string> metadata;
    for (int64 i = 0; i < metadata_tensor->NumElements(); i++) {
      metadata.push_back(metadata_tensor->flat<tstring>()(i));
    }

    OP_REQUIRES_OK(context, resource_->Init(topics, metadata, options_,
                                            input_pb_type_, output_pb_type_));
  }

  Status CreateResource(KafkaGroupReadableResource** resource)
      TF_EXCLUSIVE_LOCKS_REQUIRED(mu_) override {
    *resource = new KafkaGroupReadableResource(env_);
    return Status::OK();
  }

 private:
  mutable mutex mu_;
  Env* env_ TF_GUARDED_BY(mu_);
  DataFormatOptions options_;
  std::string output_pb_type_;
  std::string input_pb_type_;
};

class KafkaGroupReadableNextOp : public OpKernel {
 public:
  explicit KafkaGroupReadableNextOp(OpKernelConstruction* context,
                                    int version = 1)
      : OpKernel(context), version_(version) {
    env_ = context->env();
  }

  void Compute(OpKernelContext* context) override {
    KafkaGroupReadableResource* resource;
    OP_REQUIRES_OK(context,
                   GetResourceFromContext(context, "input", &resource));
    core::ScopedUnref unref(resource);

    const Tensor* index_tensor;
    OP_REQUIRES_OK(context, context->input("index", &index_tensor));
    const int64 index = index_tensor->scalar<int64>()();

    const Tensor* message_poll_timeout_tensor;
    OP_REQUIRES_OK(context, context->input("message_poll_timeout",
                                           &message_poll_timeout_tensor));
    const int64 message_poll_timeout =
        message_poll_timeout_tensor->scalar<int64>()();

    const Tensor* stream_timeout_tensor;
    OP_REQUIRES_OK(context,
                   context->input("stream_timeout", &stream_timeout_tensor));
    const int64 stream_timeout = stream_timeout_tensor->scalar<int64>()();

    OP_REQUIRES_OK(
        context,
        resource->Next(
            index, message_poll_timeout, stream_timeout,
            [&](const TensorShape& shape, Tensor** message, Tensor** key,
                Tensor** continue_fetch) -> Status {
              TF_RETURN_IF_ERROR(context->allocate_output(0, shape, message));
              if (version_ == 2) {
                TF_RETURN_IF_ERROR(context->allocate_output(1, TensorShape({}),
                                                            continue_fetch));
              } else {
                TF_RETURN_IF_ERROR(context->allocate_output(1, shape, key));
                TF_RETURN_IF_ERROR(context->allocate_output(2, TensorShape({}),
                                                            continue_fetch));
              }
              return Status::OK();
            }));
  }

 private:
  int version_ = 1;
  mutable mutex mu_;
  Env* env_ TF_GUARDED_BY(mu_);
};

class KafkaGroupReadableNextOpV2 : public KafkaGroupReadableNextOp {
 public:
  explicit KafkaGroupReadableNextOpV2(OpKernelConstruction* context)
      : KafkaGroupReadableNextOp(context, 2) {}
};

namespace {
REGISTER_KERNEL_BUILDER(Name("KafkaGroupReadableInit").Device(DEVICE_CPU),
                        KafkaGroupReadableInitOp);

REGISTER_KERNEL_BUILDER(Name("KafkaGroupReadableNext").Device(DEVICE_CPU),
                        KafkaGroupReadableNextOp);
REGISTER_KERNEL_BUILDER(Name("KafkaGroupReadableNextV2").Device(DEVICE_CPU),
                        KafkaGroupReadableNextOpV2);
}  // namespace
}  // namespace monolith_tf
}  // namespace tensorflow
