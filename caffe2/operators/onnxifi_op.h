#pragma once

#include <unordered_map>

#include "onnx/onnx_pb.h"

#include "caffe2/core/context.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/onnx/onnxifi_graph_info.h"
#include "caffe2/onnx/onnxifi_init.h"
#include "caffe2/utils/string_utils.h"

namespace caffe2 {

template <typename T, typename Context>
class OnnxifiOp final : public Operator<Context> {
  struct TensorInfo {
    TensorInfo() {}
    TensorInfo(TensorInfo&&) = default;
    TensorInfo& operator=(TensorInfo&&) = default;
    std::vector<uint64_t> dims;
    uint64_t onnxifi_type;
  };

 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  OnnxifiOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws) {
    lib_ = onnx::initOnnxifiLibrary();
    backend_graph_map_ptr_ = onnx::getOnnxBackendGraphMap();
    CAFFE_ENFORCE(lib_, "Cannot initialize ONNXIFI library");
    auto onnx_model_str =
        this->template GetSingleArgument<std::string>("onnx_model", "");
    CAFFE_ENFORCE(!onnx_model_str.empty(), "onnx_model cannot be empty");

    // Setup input/output descriptor templates
    input_names_ =
        this->template GetRepeatedArgument<std::string>("input_names");
    output_names_ =
        this->template GetRepeatedArgument<std::string>("output_names");
    CAFFE_ENFORCE_EQ(input_names_.size(), operator_def.input_size());
    CAFFE_ENFORCE_EQ(output_names_.size(), operator_def.output_size());
    for (const auto& input : input_names_) {
      input_desc_.push_back(onnxTensorDescriptorV1());
      input_desc_.back().name = input.c_str();
    }
    int output_idx = 0;
    for (const auto& output : output_names_) {
      output_desc_.push_back(onnxTensorDescriptorV1());
      output_desc_.back().name = output.c_str();

      // For output, we try to get its output size hint
      const std::string key = c10::str("output_shape_hint_", output_idx);
      auto output_shape_hint = this->template GetRepeatedArgument<int>(key);
      if (!output_shape_hint.empty()) {
        TensorInfo info;
        info.onnxifi_type = output_shape_hint.front();
        for (int i = 1; i < output_shape_hint.size(); ++i) {
          info.dims.push_back(output_shape_hint[i]);
        }
        output_shape_hints_.emplace(output_idx, std::move(info));
      }
      ++output_idx;
    }

    // Encode arguments starting with "custom_" to backend
    std::vector<uint64_t> property_pointers;
    std::vector<int64_t> int_args;
    std::vector<float> float_args;
    BuildPropertyList(operator_def, &property_pointers, &int_args, &float_args);

    // Pull the weights from workspace and feed it to the backend through
    // setGraphIO. Notice that since we may have rewritten the net, we need to
    // map the weight names
    auto initializers =
        this->template GetRepeatedArgument<std::string>("initializers");
    CAFFE_ENFORCE_EQ(
        initializers.size() % 2, 0, "initializers should come in pairs");
    std::unordered_set<std::string> initializer_set;
    std::unordered_map<std::string, std::string> input_mapping;
    for (auto it = initializers.begin(); it != initializers.end(); ++it) {
      auto key = *it++;
      input_mapping.emplace(key, *it);
      initializer_set.emplace(key);
    }
    Workspace mapped_ws(ws, input_mapping);
    std::vector<std::string> weight_names;
    std::vector<std::vector<uint64_t>> weight_shapes;
    auto weight_descs = buildInitializationList(
        &mapped_ws, &initializer_set, &weight_names, &weight_shapes);

    BuildBackendAndGraph(property_pointers, onnx_model_str, weight_descs);
  }

  ~OnnxifiOp() {
    backend_graph_shared_ptr_.reset();
    backend_graph_map_ptr_->remove(op_id_string_);
  }

  bool RunOnDevice() override;

 private:
  uint64_t SetOutputShapeAndType(int output_idx, std::vector<size_t>* dims) {
    uint64_t type = ONNXIFI_DATATYPE_FLOAT32;
    const auto it = output_shape_hints_.find(output_idx);
    if (it != output_shape_hints_.end()) {
      std::copy(
          it->second.dims.begin(),
          it->second.dims.end(),
          std::back_inserter(*dims));
      type = it->second.onnxifi_type;
    }
    return type;
  }

  void BuildPropertyList(
      const OperatorDef& /* unused */,
      std::vector<uint64_t>* property_list,
      std::vector<int64_t>* /* unused */,
      std::vector<float>* /* unused */) {
    property_list->push_back(ONNXIFI_BACKEND_PROPERTY_NONE);
  }

  void BuildBackendAndGraph(
      const std::vector<uint64_t>& property_pointers,
      const std::string& onnx_model_str,
      const std::vector<onnxTensorDescriptorV1>& weight_descs) {
    op_id_string_ =
        this->template GetSingleArgument<std::string>("model_id", "") + ":" +
        this->template GetSingleArgument<std::string>("net_pos", "");

    // Build the Onnxifi engine
    auto backend_index = this->template GetSingleArgument<int>("backend_id", 0);
    onnxifi_library* lib = lib_;
    auto creator = [lib,
                    property_pointers,
                    backend_index,
                    &onnx_model_str,
                    &weight_descs]() {
      std::vector<onnxBackendID> backend_ids;
      size_t num_backends{0};
      CAFFE_ENFORCE_EQ(
          lib->onnxGetBackendIDs(nullptr, &num_backends),
          ONNXIFI_STATUS_FALLBACK);
      CAFFE_ENFORCE_GT(
          num_backends, 0, "At least 1 onnxifi backend should be available");
      CAFFE_ENFORCE_LT(
          backend_index,
          num_backends,
          "Backend idx out of bound: ",
          backend_index,
          ", #backends: ",
          num_backends);
      backend_ids.resize(num_backends);
      CAFFE_ENFORCE_EQ(
          lib->onnxGetBackendIDs(backend_ids.data(), &num_backends),
          ONNXIFI_STATUS_SUCCESS);

      onnxBackendID backend_id = backend_ids[backend_index];
      onnxBackend backend{nullptr};

      CAFFE_ENFORCE_EQ(
          lib->onnxInitBackend(backend_id, property_pointers.data(), &backend),
          ONNXIFI_STATUS_SUCCESS);

      // Release unused backend ids.
      for (auto i = 0; i < num_backends; ++i) {
        if (i == backend_index) {
          continue;
        }
        lib->onnxReleaseBackendID(backend_ids[i]);
      }
      onnxGraph graph{nullptr};
      CAFFE_ENFORCE_EQ(
          lib->onnxInitGraph(
              backend,
              nullptr,
              onnx_model_str.size(),
              (const void*)(onnx_model_str.c_str()),
              weight_descs.size(),
              weight_descs.data(),
              &graph),
          ONNXIFI_STATUS_SUCCESS);

      return std::make_shared<onnx::BackendGraphInfo>(
          backend_id, backend, graph, lib);
    };
    backend_graph_shared_ptr_ =
        backend_graph_map_ptr_->insert(op_id_string_, creator);

    backend_id_ = backend_graph_shared_ptr_->backend_id;
    backend_ = backend_graph_shared_ptr_->backend;
    graph_ = backend_graph_shared_ptr_->graph;

// Set up function pointer if onnxifi_ext is enabled
#ifdef ONNXIFI_ENABLE_EXT
    onnxExtensionFunctionPointer p;
    if (lib_->onnxGetExtensionFunctionAddress(
            backend_id_, "onnxSetIOAndRunGraphFunction", &p) !=
        ONNXIFI_STATUS_SUCCESS) {
      onnxSetIOAndRunGraphPointer_ = nullptr;
      return;
    }
    onnxSetIOAndRunGraphPointer_ =
        reinterpret_cast<decltype(onnxSetIOAndRunGraphPointer_)>(p);
#endif
  }

  std::vector<onnxTensorDescriptorV1> buildInitializationList(
      Workspace* ws,
      std::unordered_set<std::string>* initialization_list,
      std::vector<std::string>* weight_names,
      std::vector<std::vector<uint64_t>>* weight_shapes);

  // pointer to loaded onnxifi library
  onnxifi_library* lib_{nullptr};
  onnx::OnnxBackendGraphMap* backend_graph_map_ptr_;
  std::string op_id_string_;

  onnxBackendID backend_id_{nullptr};
  onnxBackend backend_{nullptr};
  onnxGraph graph_{nullptr};
  onnx::SharedPtrBackendGraphInfo backend_graph_shared_ptr_;

  // input/output descriptors
  std::vector<onnxTensorDescriptorV1> input_desc_;
  std::vector<onnxTensorDescriptorV1> output_desc_;

#ifdef ONNXIFI_ENABLE_EXT
  // onnxifi extension mode function pointer
  onnxStatus (*onnxSetIOAndRunGraphPointer_)(
      onnxGraph,
      uint32_t,
      const onnxTensorDescriptorV1*,
      uint32_t,
      const onnxTensorDescriptorV1*,
      onnxMemoryFenceV1*);
#endif

  // We bind the op input/output by position while ONNXIFI binds input/output by
  // names. In addition, op input/output names can be writtten by, for example,
  // memonger. We cache the original input/output name of ONNX object here and
  // bind them by position.
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;

  std::vector<std::vector<uint64_t>> input_shapes_;
  std::vector<std::vector<uint64_t>> output_shapes_;

  // output shape hints
  std::unordered_map<int, TensorInfo> output_shape_hints_;
};

} // namespace caffe2
