/*
 * Copyright (C) 2020 ByteDance Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef GOOGLE_CUDA
#define EIGEN_USE_GPU
#include "bert_transformer_op.h"
#include "common.h"
#include "cuda/attention.h"
#include "cuda/cuda_kernels.h"
#include "tensorflow/core/framework/op.h"
#include <cuda_runtime.h>
#include <string>
#include <stdlib.h>
#include <vector>

#include "tensorflow/compiler/xla/service/custom_call_target_registry.h"


namespace tensorflow
{
using GPUDevice = Eigen::GpuDevice;
using namespace effectivetransformer;

std::vector<std::string> split(const std::string& str, char delim = ' ')
{   
    std::vector<std::string> cont;
    std::size_t current, previous = 0;
    current = str.find(delim);
    while (current != std::string::npos) {
        cont.push_back(str.substr(previous, current - previous));
        previous = current + 1;
        current = str.find(delim, previous);
    }
    cont.push_back(str.substr(previous, current - previous));
    return cont;
}

void exclusiveScan(CUstream stream, void** buffers,
                    const char* opaque, size_t opaque_len) {
  int prefix_sum_real_size = atoi(opaque);
  const int* mask_buf = reinterpret_cast<int*>(buffers[0]);
  int* prefix_sum_buf = reinterpret_cast<int*>(buffers[1]);
  exclusiveScan_kernelLauncher(prefix_sum_buf, mask_buf, prefix_sum_real_size, stream);
}

XLA_REGISTER_CUSTOM_CALL_TARGET(exclusiveScan, "CUDA");

template <typename T>
void compressBertInput(CUstream stream, void** buffers,
                    const char* opaque, size_t opaque_len) {
  typedef typename TransformerTFTraits<T>::DataType DataType_;

  std::string sizes(opaque);
  std::vector<std::string> sizes_3 = split(sizes);
  int batch_size = std::stoi(sizes_3[0]);
  int from_seq_len = std::stoi(sizes_3[1]);
  int hidden_size = std::stoi(sizes_3[2]);
  int word_num = batch_size * from_seq_len;

  const DataType_ * from_tensor = reinterpret_cast<DataType_ *>(buffers[0]);
  const int* mask = reinterpret_cast<int*>(buffers[1]);
  const int* prefix_sum = reinterpret_cast<int*>(buffers[2]);
  DataType_ * to_tensor = reinterpret_cast<DataType_ *>(buffers[3]);
  int* valid_word_num = reinterpret_cast<int*>(buffers[4]);
  int* batch_idx = reinterpret_cast<int*>(buffers[5]);
  int* word_idx = reinterpret_cast<int*>(buffers[6]);

  compressBertInput_kernelLauncher(
        from_tensor, mask, prefix_sum,
        to_tensor, batch_idx, word_idx,
        batch_size ,from_seq_len, hidden_size, stream);

  int valid_word_num_host = 0;
  int last_mask_host = 0;
  check_cuda_error(cudaMemcpyAsync(
    &valid_word_num_host, prefix_sum + word_num - 1, sizeof(int), cudaMemcpyDeviceToHost, stream));
  check_cuda_error(cudaMemcpyAsync(
    &last_mask_host, mask + word_num - 1, sizeof(int), cudaMemcpyDeviceToHost, stream));
  check_cuda_error(cudaStreamSynchronize(stream));
  if (last_mask_host == 1) {
    // in case of the last mask is 1, since this is exclusive scan
    valid_word_num_host ++;
  }
  check_cuda_error(cudaMemcpyAsync(
        valid_word_num, &valid_word_num_host, sizeof(int), cudaMemcpyHostToDevice, stream));
}


XLA_REGISTER_CUSTOM_CALL_TARGET_WITH_SYM("compressBertInput" + std::string(typeid(float).name()), compressBertInput<float>, "CUDA");
XLA_REGISTER_CUSTOM_CALL_TARGET_WITH_SYM("compressBertInput" + std::string(typeid(Eigen::half).name()), compressBertInput<Eigen::half>, "CUDA");


namespace functor
{
template <typename T>
struct BertTransformerOpFunctor<GPUDevice, T>
{
  typedef typename TransformerTFTraits<T>::DataType DataType_;
  static Status Compute(OpKernelContext *context,
                        EncoderInitParam<DataType_ > param,
                        TransformerParam t_param)
  {
    const cudaStream_t &stream = context->eigen_device<GPUDevice>().stream();
    param.stream = stream;
    try
    {
      check_cuda_error(cublasSetStream(param.cublas_handle, stream));

      /// 1. Set compute type
      cudaDataType_t computeType, AType, BType, CType;
      int cublasAlgo[3];
      if (TransformerTFTraits<T>::OpType == OperationType::FP32) {
        computeType = CUDA_R_32F;
        AType = CUDA_R_32F;
        BType = CUDA_R_32F;
        CType = CUDA_R_32F;
        cublasAlgo[0] = -1;
        cublasAlgo[1] = -1;
        cublasAlgo[2] = -1;
      } else {
        computeType = CUDA_R_16F;
        AType = CUDA_R_16F;
        BType = CUDA_R_16F;
        CType = CUDA_R_16F;
        cublasAlgo[0] = 99;
        cublasAlgo[1] = 99;
        cublasAlgo[2] = 99;
      }
      DataType_ alpha = (DataType_)1.0f, beta = (DataType_)0.0f;

      /// 2. allocate buffer for transformer
      Tensor buf_tensor;
      int batch_size     = t_param.batch_size_;
      int head_num       = t_param.head_num_;
      int from_seq_len   = t_param.from_seq_len_;
      int size_per_head  = t_param.size_per_head_;
      int input_tensor_size = batch_size * head_num * from_seq_len * size_per_head;
      int attn_tensor_size  = batch_size * head_num * from_seq_len * from_seq_len;
      long long int buf_size = input_tensor_size * 13 + attn_tensor_size;

      tensorflow::Status status = context->allocate_temp(DT_UINT8, TensorShape{buf_size}, &buf_tensor);

      /// 3. assign intermediate pointer
      DataType_* buf = reinterpret_cast<DataType_ *>(buf_tensor.flat<uint8>().data());
      /// buffer for qkv
      DataType_* query_buf_     = buf + 0 * input_tensor_size;
      DataType_* key_buf_       = buf + 1 * input_tensor_size;
      DataType_* value_buf_     = buf + 2 * input_tensor_size;
      DataType_* query_         = buf + 3 * input_tensor_size;
      DataType_* key_           = buf + 4 * input_tensor_size;
      DataType_* value_         = buf + 5 * input_tensor_size;
      /// buffer for self attention
      DataType_* qk_buf_           = buf + 0 * input_tensor_size;
      DataType_* transpose_dst_    = buf + std::max(attn_tensor_size, input_tensor_size);
      /// buffer for output matmat
      DataType_* attr_out_buf_     = buf + 0 * input_tensor_size;
      DataType_* attr_matmul_buf_  = buf + 1 * input_tensor_size;
      DataType_* inter_matmul_buf_ = buf + 2 * input_tensor_size;

      /// 4. get valid word num
      int valid_word_num = 0;
      check_cuda_error(cudaMemcpyAsync(
        &valid_word_num, param.valid_word_num, sizeof(int), cudaMemcpyDeviceToHost, param.stream));
      // std::cout << "valid_word_num : " << valid_word_num << std::endl;

      // 5. input -> Q K V
      {
        int m = valid_word_num;
        int k = t_param.head_num_ * t_param.size_per_head_;
        int n = k;

        check_cuda_error(cublasGemmEx(param.cublas_handle,
          CUBLAS_OP_N, CUBLAS_OP_N,
          n, m, k,
          &alpha,
          param.attr_kernel_Q, AType, n,
          param.from_tensor, BType, k,
          &beta,
          query_buf_, CType, n,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[0])));

        check_cuda_error(cublasGemmEx(param.cublas_handle,
          CUBLAS_OP_N, CUBLAS_OP_N,
          n, m, k,
          &alpha,
          param.attr_kernel_K, AType, n,
          param.to_tensor, BType, k,
          &beta,
          key_buf_, CType, n,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[0])));

        check_cuda_error(cublasGemmEx(param.cublas_handle,
          CUBLAS_OP_N, CUBLAS_OP_N,
          n, m, k,
          &alpha,
          param.attr_kernel_V, AType, n,
          param.to_tensor, BType, k,
          &beta,
          value_buf_, CType, n,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[0])));

        // check_cuda_error(cudaMemsetAsync(query_, 0, input_tensor_size * sizeof(DataType_), stream));
        // check_cuda_error(cudaMemsetAsync(key_,   0, input_tensor_size * sizeof(DataType_), stream));
        // check_cuda_error(cudaMemsetAsync(value_, 0, input_tensor_size * sizeof(DataType_), stream));
        check_cuda_error(cudaMemsetAsync(query_, 0, 3 * input_tensor_size * sizeof(DataType_), stream));

        /// add bias & add padding & transpose for self-attention
        cuda::add_QKV_bias_padding_kernelLauncher<DataType_>(
          query_buf_, param.attr_bias_Q,
          key_buf_, param.attr_bias_K,
          value_buf_, param.attr_bias_V,
          query_, key_, value_,
          valid_word_num, batch_size, from_seq_len, head_num, size_per_head,
          param.batch_idx, param.word_idx, stream);
      }

      /// 6. self-attention
      {
        check_cuda_error(cublasGemmStridedBatchedEx(param.cublas_handle,
          CUBLAS_OP_T, CUBLAS_OP_N,
          from_seq_len, from_seq_len, size_per_head,
          &alpha,
          key_, AType, size_per_head, from_seq_len * size_per_head,
          query_, BType, size_per_head, from_seq_len * size_per_head,
          &beta,
          qk_buf_, CType, from_seq_len, from_seq_len * from_seq_len,
          batch_size * head_num,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[1])));

        DataType_ scaler = 1 / sqrtf(size_per_head * 1.0f);
        cuda::softmax_kernel_kernelLauncher<DataType_>(
          qk_buf_, param.attr_mask, batch_size, head_num, from_seq_len, scaler, stream);

        check_cuda_error(cublasGemmStridedBatchedEx(param.cublas_handle,
          CUBLAS_OP_N, CUBLAS_OP_N,
          size_per_head, from_seq_len, from_seq_len,
          &alpha,
          value_, AType, size_per_head, from_seq_len * size_per_head,
          qk_buf_, BType, from_seq_len, from_seq_len * from_seq_len,
          &beta,
          transpose_dst_, CType, size_per_head, from_seq_len * size_per_head,
          batch_size * head_num,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[2])));

        cuda::transpose_rm_padding_kernelLauncher<DataType_>(
          transpose_dst_, attr_out_buf_,
          valid_word_num, batch_size, from_seq_len, head_num, size_per_head,
          param.batch_idx, param.word_idx, stream);
      }

      /// 7. matmat & layer norm
      {
        int m = valid_word_num;
        int k = head_num * size_per_head;
        int n = k;

        check_cuda_error(cublasGemmEx(param.cublas_handle,
          CUBLAS_OP_N, CUBLAS_OP_N,
          n, m, k,
          &alpha,
          param.attr_output_kernel, AType, n,
          attr_out_buf_, BType, k,
          &beta,
          attr_matmul_buf_, CType, n,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[0])));

        add_bias_input_layernorm_kernelLauncher<DataType_>(attr_matmul_buf_,
          param.from_tensor, param.attr_output_bias, param.attr_output_layernorm_gamma,
          param.attr_output_layernorm_beta, m, n, param.stream);

        n *= 4;
        check_cuda_error(cublasGemmEx(param.cublas_handle,
          CUBLAS_OP_N, CUBLAS_OP_N,
          n, m, k,
          &alpha,
          param.inter_kernel, AType, n,
          attr_matmul_buf_, BType, k,
          &beta,
          inter_matmul_buf_, CType, n,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[1])));

        add_bias_act_kernelLauncher<DataType_>(inter_matmul_buf_, param.inter_bias, m, n, param.stream);

        n = k;
        k *= 4;
        check_cuda_error(cublasGemmEx(param.cublas_handle,
          CUBLAS_OP_N, CUBLAS_OP_N,
          n, m, k,
          &alpha,
          param.output_kernel, AType, n,
          inter_matmul_buf_, BType, k,
          &beta,
          param.transformer_out, CType, n,
          computeType,
          static_cast<cublasGemmAlgo_t>(cublasAlgo[2])));

        add_bias_input_layernorm_kernelLauncher<DataType_>(
            param.transformer_out, attr_matmul_buf_, param.output_bias,
            param.output_layernorm_gamma,
            param.output_layernorm_beta,
            m, n, param.stream);
      }

      return Status::OK();
    }
    catch(std::runtime_error& error)
    {
      return errors::Internal(error.what());
    }
    catch(...)
    {
      return errors::Internal("Runtime error");
    }
  }
};

template struct functor::BertTransformerOpFunctor<GPUDevice, float>;
template struct functor::BertTransformerOpFunctor<GPUDevice, Eigen::half>;

/// ************************* Transformer input parser *************************
template <typename T>
struct BertTransformerInputOpFunctor<GPUDevice, T>
{
  typedef typename TransformerTFTraits<T>::DataType DataType_;
  static Status Compute(OpKernelContext *context,
                        EncoderInputInitParam<DataType_ > param)
  {
    const cudaStream_t &stream = context->eigen_device<GPUDevice>().stream();
    param.stream = stream;
    try
    {
      /// 1. allocate tf temp memory
      Tensor buf;
      long long int buf_size = param.batch_size_ * param.from_seq_len_ * 2;
      tensorflow::Status status = context->allocate_temp(DT_INT32, TensorShape{buf_size}, &buf);
      int* prefix_sum_buf_ = reinterpret_cast<int *>(buf.flat<int>().data());

      if(status != tensorflow::Status::OK()) {
        throw std::runtime_error("TF error: context->allocate_temp failed");
      }

      if(prefix_sum_buf_ == nullptr) {
        throw std::runtime_error(std::string("Tensorflow Allocator failed to allocate internal buffer."));
      }

      /// 2. compute mask's prefix sum
      int word_num = param.batch_size_ * param.from_seq_len_;
      exclusiveScan_kernelLauncher(prefix_sum_buf_, param.mask, word_num, param.stream);

      /// 3. compress input tensor, copy tensor according to there
      ///    will be faster if placed in front of embedding ...
      /// nv's thrust::copy_if is very slow...
      compressBertInput_kernelLauncher(
        param.from_tensor, param.mask, prefix_sum_buf_,
        param.to_tensor, param.batch_idx, param.word_idx,
        param.batch_size_ ,param.from_seq_len_, param.head_num_ * param.size_per_head_, param.stream);

      /// 2. get valid word num
      int valid_word_num = 0;
      int last_mask = 0;
      check_cuda_error(cudaMemcpyAsync(
        &valid_word_num, prefix_sum_buf_ + word_num - 1, sizeof(int), cudaMemcpyDeviceToHost, param.stream));
      check_cuda_error(cudaMemcpyAsync(
        &last_mask, param.mask + word_num - 1, sizeof(int), cudaMemcpyDeviceToHost, param.stream));
      check_cuda_error(cudaStreamSynchronize(param.stream));

      if (last_mask == 1) {
        // in case of the last mask is 1, since this is exclusive scan
        valid_word_num ++;
      }
      check_cuda_error(cudaMemcpyAsync(
        param.valid_word_num, &valid_word_num, sizeof(int), cudaMemcpyHostToDevice, param.stream));
      // std::cout << "valid_word_num : " << valid_word_num << std::endl;

      return Status::OK();
    }
    catch(std::runtime_error& error)
    {
      return errors::Internal(error.what());
    }
    catch(...)
    {
      return errors::Internal("Runtime error");
    }
  }
};

template struct functor::BertTransformerInputOpFunctor<GPUDevice, float>;
template struct functor::BertTransformerInputOpFunctor<GPUDevice, Eigen::half>;



/// ************************* Transformer output parser *************************
template <typename T>
struct BertTransformerOutputOpFunctor<GPUDevice, T>
{
  typedef typename TransformerTFTraits<T>::DataType DataType_;
  static Status Compute(OpKernelContext *context,
                        EncoderOutputInitParam<DataType_ > param)
  {
    const cudaStream_t &stream = context->eigen_device<GPUDevice>().stream();
    param.stream = stream;
    try
    {
      int valid_word_num = 0;
      check_cuda_error(cudaMemcpyAsync(
        &valid_word_num, param.valid_word_num, sizeof(int), cudaMemcpyDeviceToHost, param.stream));

      int tensor_size = param.batch_size_ * param.head_num_ * param.from_seq_len_ * param.size_per_head_;
      check_cuda_error(cudaMemsetAsync(param.to_tensor, 0, tensor_size * sizeof(DataType_), param.stream));

      restoreBertOutput_kernelLauncher(
        param.to_tensor,
        param.from_tensor, param.batch_idx, param.word_idx,
        valid_word_num, param.from_seq_len_, param.head_num_ * param.size_per_head_, param.stream);

      return Status::OK();
    }
    catch(std::runtime_error& error)
    {
      return errors::Internal(error.what());
    }
    catch(...)
    {
      return errors::Internal("Runtime error");
    }
  }
};
template struct functor::BertTransformerOutputOpFunctor<GPUDevice, float>;
template struct functor::BertTransformerOutputOpFunctor<GPUDevice, Eigen::half>;

} //namespace functor

} //namespace tensorflow
#endif


// { ////////////////////////////////////////////////////////////////////////////////
//   // printf("input_tensor_size : %d\n", input_tensor_size);
//   printf("attn final : \n");
//   printf("transpose valid_word_num : %d, batch_size : %d, from_seq_len : %d, head_num : %d, size_per_head : %d\n",
//           valid_word_num, batch_size, from_seq_len, head_num, size_per_head);
//   __half* tmp = new __half[input_tensor_size];
//   check_cuda_error(cudaMemcpyAsync(
//     tmp, attr_out_buf_, input_tensor_size * sizeof(__half), cudaMemcpyDeviceToHost, stream));

//   int word = 1;
//   std::vector<int> len = {16, 24, 33, 8};
//   for (int bi = 0; bi < batch_size; bi++) {
//     int b_off = 0;
//     for (int bbi = 0; bbi < bi; bbi++)
//       b_off += len[bbi];
//     int offset = b_off * 768 + word * 768;
//     printf("batch %d : %f %f %f ... %f %f %f\n", bi,
//         (float)(tmp[offset + 0]),
//         (float)(tmp[offset + 1]),
//         (float)(tmp[offset + 2]),
//         (float)(tmp[offset + 768 - 3]),
//         (float)(tmp[offset + 768 - 2]),
//         (float)(tmp[offset + 768 - 1]));
//   }
//   delete [] tmp;
// } ////////////////////////////////////////////////////////////////////////////////
