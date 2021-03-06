/*
 * Copyright (c) 2020-2021, NVIDIA CORPORATION.  All rights reserved.
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

#define EIGEN_USE_GPU

#include "fastertransformer/open_decoder.h"
#include "fastertransformer/gpt.h"
#include "fastertransformer/utils/common.h"

#include "fastertransformer/tf_op/common_op.h"
#include "fastertransformer/tf_op/tf_traits.h"

namespace tensorflow
{
namespace
{
using CPUDevice = Eigen::ThreadPoolDevice;
using GPUDevice = Eigen::GpuDevice;

REGISTER_OP("DecodingGPT")
    .Input("self_beta: T")                  // 0
    .Input("self_gamma: T")                 // 1
    .Input("self_q_kernel: T")              // 2
    .Input("self_q_bias: T")                // 3
    .Input("self_k_kernel: T")              // 4
    .Input("self_k_bias: T")                // 5
    .Input("self_v_kernel: T")              // 6
    .Input("self_v_bias: T")                // 7
    .Input("self_output_kernel: T")         // 8
    .Input("self_output_bias: T")           // 9
    .Input("ffn_beta: T")                   // 10
    .Input("ffn_gamma: T")                  // 11
    .Input("ffn_kernel1: T")                // 12
    .Input("ffn_bias1: T")                  // 13
    .Input("ffn_kernel2: T")                // 14
    .Input("ffn_bias2: T")                  // 15
    .Input("decoding_beta: T")              // 16
    .Input("decoding_gamma: T")             // 17
    .Input("embedding_table: T")            // 18
    .Input("embedding_kernel: T")           // 19
    .Input("position_encoding_table: T")    // 20
    .Input("attention_mask: T")             // 21
    .Input("start_ids: int32")              // 22
    .Input("min_start_length: int32")       // 23
    .Input("max_start_length: int32")       // 24
    .Input("start_lengths: int32")          // 25
    .Output("output_ids: int32")
    .Attr("T: {float, half}")
    .Attr("batch_size: int >= 1")
    .Attr("candidate_num: int >= 0")
    .Attr("probability_threshold: float = 0.0")
    .Attr("max_seq_len: int >= 1")
    .Attr("head_num: int >= 1")
    .Attr("size_per_head: int >= 1")
    .Attr("num_layer: int >= 1")
    .Attr("start_id: int >= 0")
    .Attr("end_id: int >= 0")
    .Attr("temperature: float = 1.0")
    .Attr("is_fuse_qkv: bool")
    .SetShapeFn([](shape_inference::InferenceContext *c) {
        int batch_size, max_seq_len;
        c->GetAttr("batch_size", &batch_size);
        c->GetAttr("max_seq_len", &max_seq_len);

        c->set_output(0, c->MakeShape({max_seq_len, batch_size}));
        return Status::OK();
    });
template <typename Device, typename T>
class DecodingGPTOp : public CommonOp<T>
{
public:
    explicit DecodingGPTOp(OpKernelConstruction *context) : CommonOp<T>(context)
    {
        OP_REQUIRES_OK(context, context->GetAttr("batch_size", &batch_size_));
        OP_REQUIRES_OK(context, context->GetAttr("candidate_num", &candidate_num_));
        OP_REQUIRES_OK(context, context->GetAttr("probability_threshold", &probability_threshold_));
        OP_REQUIRES_OK(context, context->GetAttr("max_seq_len", &max_seq_len_));
        OP_REQUIRES_OK(context, context->GetAttr("head_num", &head_num_));
        OP_REQUIRES_OK(context, context->GetAttr("size_per_head", &size_per_head_));
        OP_REQUIRES_OK(context, context->GetAttr("num_layer", &num_layer_));
        OP_REQUIRES_OK(context, context->GetAttr("start_id", &start_id_));
        OP_REQUIRES_OK(context, context->GetAttr("end_id", &end_id_));
        OP_REQUIRES_OK(context, context->GetAttr("temperature", &temperature_));
        OP_REQUIRES_OK(context, context->GetAttr("is_fuse_qkv", &is_fuse_qkv_));
#ifndef NDEBUG
        srand(0);
        printf("[WARNING] Fixing the random seed in FasterTransformer OP. \n");
#else
        srand(time(NULL));
#endif
        
    }

    void Compute(OpKernelContext *context) override
    {
        const int vocab_size = (int)context->input(18).dim_size(0);

        DecodingInitParam<DataType_> decoding_params;
        decoding_params.cublas_handle = this->get_cublas_handler();
        decoding_params.cublaslt_handle = this->get_cublaslt_handler();
        Tensor *output_ids = nullptr;
        OP_REQUIRES_OK(
            context,
            context->allocate_output(0, {max_seq_len_, batch_size_}, &output_ids));

        decoding_params.output_ids = reinterpret_cast<int *>(output_ids->flat<int>().data());

        check_cuda_error(cudaMemset(decoding_params.output_ids, 0, sizeof(int) * max_seq_len_ * batch_size_));

        typedef DecoderTransformerTraits<traits_::OpType> DecodingTraits_;
        DecodingGpt<DecodingTraits_::OpType> *decoding_handler;
        const cudaStream_t &stream = context->eigen_device<Device>().stream();
        decoding_params.stream = stream;
        fastertransformer::Allocator<AllocatorType::TF> allocator_(context, stream);
        try
        {
            decoding_handler = new DecodingGpt<DecodingTraits_::OpType>(
                allocator_, batch_size_, 
                max_seq_len_, head_num_, size_per_head_,
                vocab_size, num_layer_,
                start_id_, end_id_,
                candidate_num_, probability_threshold_, temperature_,
                1, 1, is_fuse_qkv_);
        }
        catch (std::runtime_error &error)
        {
            OP_REQUIRES(context, false, errors::Internal(error.what()));
        }

        OP_REQUIRES(context, context->num_inputs() <= 26, errors::InvalidArgument("[ERROR] Require less input arguments"));
        OP_REQUIRES(context, context->num_inputs() >= 26, errors::InvalidArgument("[ERROR] Require more input arguments"));

        DecoderInitParam<DataType_> *params = new DecoderInitParam<DataType_>[num_layer_];
        const int hidden_unit = size_per_head_ * head_num_;
        for (int i = 0; i < num_layer_; i++)
        {
            params[i].request_batch_size = batch_size_;
            params[i].stream = stream;
            params[i].cublas_handle = this->get_cublas_handler();
            params[i].cublaslt_handle = this->get_cublaslt_handler();
            check_cuda_error(cublasSetStream(params[i].cublas_handle, params[i].stream));

            this->get_tensor(context, 0, &params[i].self_layernorm.beta, i * hidden_unit);
            this->get_tensor(context, 1, &params[i].self_layernorm.gamma, i * hidden_unit);

            if(is_fuse_qkv_)
            {
                this->get_tensor(context, 2, &params[i].self_attention.query_weight.kernel, i * hidden_unit * hidden_unit * 3);
                this->get_tensor(context, 3, &params[i].self_attention.query_weight.bias, i * hidden_unit * 3);
            }
            else
            {
                this->get_tensor(context, 2, &params[i].self_attention.query_weight.kernel, i * hidden_unit * hidden_unit);
                this->get_tensor(context, 3, &params[i].self_attention.query_weight.bias, i * hidden_unit);
                this->get_tensor(context, 4, &params[i].self_attention.key_weight.kernel, i * hidden_unit * hidden_unit);
                this->get_tensor(context, 5, &params[i].self_attention.key_weight.bias, i * hidden_unit);
                this->get_tensor(context, 6, &params[i].self_attention.value_weight.kernel, i * hidden_unit * hidden_unit);
                this->get_tensor(context, 7, &params[i].self_attention.value_weight.bias, i * hidden_unit);
            }

            this->get_tensor(context, 8, &params[i].self_attention.attention_output_weight.kernel, i * hidden_unit * hidden_unit);
            this->get_tensor(context, 9, &params[i].self_attention.attention_output_weight.bias, i * hidden_unit);

            this->get_tensor(context, 10, &params[i].ffn_layernorm.beta, i * hidden_unit);
            this->get_tensor(context, 11, &params[i].ffn_layernorm.gamma, i * hidden_unit);
            this->get_tensor(context, 12, &params[i].ffn.intermediate_weight.kernel, i * hidden_unit * hidden_unit * 4);
            this->get_tensor(context, 13, &params[i].ffn.intermediate_weight.bias, i * hidden_unit * 4);
            this->get_tensor(context, 14, &params[i].ffn.output_weight.kernel, i * hidden_unit * hidden_unit * 4);
            this->get_tensor(context, 15, &params[i].ffn.output_weight.bias, i * hidden_unit);
        }

        this->get_tensor(context, 16, &decoding_params.layernorm.beta);
        this->get_tensor(context, 17, &decoding_params.layernorm.gamma);
        this->get_tensor(context, 18, &decoding_params.embedding_table);
        this->get_tensor(context, 19, &decoding_params.embedding_kernel);
        this->get_tensor(context, 20, &decoding_params.position_encoding_table);

        const DataType_* d_attn_mask = nullptr;
        this->get_tensor(context, 21, &d_attn_mask);


        const int* d_start_ids = reinterpret_cast<const int *>(context->input(22).flat<int32>().data());
        OP_REQUIRES(context, d_start_ids != nullptr, errors::InvalidArgument("d_start_ids is null"));
        const int* d_min_start_length = reinterpret_cast<const int *>(context->input(23).flat<int32>().data());
        OP_REQUIRES(context, d_min_start_length != nullptr, errors::InvalidArgument("d_min_start_length is null"));
        const int* d_max_start_length = reinterpret_cast<const int *>(context->input(24).flat<int32>().data());
        OP_REQUIRES(context, d_max_start_length != nullptr, errors::InvalidArgument("d_max_start_length is null"));
        int min_start_length = -1;
        int max_start_length = -1;
        cudaMemcpyAsync(&min_start_length, d_min_start_length, sizeof(int), cudaMemcpyDeviceToHost, stream);
        cudaMemcpyAsync(&max_start_length, d_max_start_length, sizeof(int), cudaMemcpyDeviceToHost, stream);
        assert(min_start_length != -1);
        assert(max_start_length != -1);
        const int* d_start_lengths = reinterpret_cast<const int *>(context->input(25).flat<int32>().data());
        OP_REQUIRES(context, d_start_lengths != nullptr, errors::InvalidArgument("d_start_lengths is null"));

        TensorParallelParam tensor_parallel_param;
        tensor_parallel_param.rank = 0;
        tensor_parallel_param.world_size = 1;
        tensor_parallel_param.local_head_num_ = head_num_;
        tensor_parallel_param.local_hidden_units_ = hidden_unit;

        LayerParallelParam layer_parallel_param;
        layer_parallel_param.rank = 0;
        layer_parallel_param.world_size = 1;
        layer_parallel_param.layers_per_group = num_layer_;
        layer_parallel_param.local_batch_size = batch_size_;
        
        decoding_params.d_attn_mask = d_attn_mask;
        decoding_params.d_start_lengths = d_start_lengths;

        decoding_handler->set_tensor_parallel_param(tensor_parallel_param);
        decoding_handler->set_layer_parallel_param(layer_parallel_param);

        decoding_params.request_batch_size = batch_size_;
        decoding_params.max_input_len = 1;
        for(int i = 0; i < decoding_handler->get_num_layer(); i++)
        {
            params[i].request_batch_size = batch_size_;
        }
        decoding_params.request_input_len = 1;

        try
        {
            decoding_handler->forward_context(params, decoding_params);
            decoding_handler->forward(params, decoding_params);
        }
        catch (std::runtime_error &error)
        {
            std::cout << errors::Internal(error.what());
            exit(-1);
        }
        catch (...)
        {
            std::cout << errors::Internal("Runtime error");
            exit(-1);
        }

        delete decoding_handler;
        delete[] params;
    }

private:
    int batch_size_, candidate_num_, max_seq_len_;
    float probability_threshold_, temperature_;
    int head_num_, size_per_head_, num_layer_;
    int start_id_, end_id_;
    bool is_fuse_qkv_;
    typedef TFTraits<T> traits_;
    typedef typename traits_::DataType DataType_;
};

#ifdef GOOGLE_CUDA

#define REGISTER_GPU(T)                                                 \
    REGISTER_KERNEL_BUILDER(                                            \
        Name("DecodingGPT").Device(DEVICE_GPU).TypeConstraint<T>("T"), \
        DecodingGPTOp<GPUDevice, T>)
REGISTER_GPU(float);
REGISTER_GPU(Eigen::half);
#undef REGISTER_GPU

#endif
} //namespace
} //namespace tensorflow
