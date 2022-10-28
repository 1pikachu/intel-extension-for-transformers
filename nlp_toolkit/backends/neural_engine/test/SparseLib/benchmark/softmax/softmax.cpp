//  Copyright (c) 2022 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "utils.hpp"
#include "softmax/softmax.hpp"

namespace jd {

void parse_sepc_softmax_args(std::string str, std::map<std::string, std::string>* args_map) {
  auto string_list = split_str<std::string>(str, ':');  // list[0]=key;list[1]=value
  if (string_list.size() != 2) LOG(ERROR) << "invalid arg.\n";
  (*args_map)[string_list[0]] = string_list[1];
}

bench_res_t softmax_bench::set_config(int argc, char** argv) {
  // parse args
  std::map<std::string, std::string> args_map;
  for (int i = 0; i < argc; i++) parse_sepc_softmax_args(argv[i], &args_map);

  // check args legitimacy
  if (args_map.find("spec_type") == args_map.end()) LOG(ERROR) << "must give spec_type arg.\n";
  if (args_map["spec_type"] != "lut") LOG(ERROR) << "unsupported spec_type: " << args_map["spec_type"] << std::endl;
  if (args_map.find("shape") == args_map.end()) LOG(ERROR) << "must give shape arg.\n";
  if (args_map.find("in_dt") == args_map.end()) LOG(ERROR) << "must give in_dt arg.\n";
  if (args_map.find("out_dt") == args_map.end()) LOG(ERROR) << "must give out_dt arg.\n";

  // gen param
  in_dt = str_2_dt(args_map["in_dt"]);
  out_dt = str_2_dt(args_map["out_dt"]);
  for (auto&& i : split_str<int>(args_map["shape"], 'x')) shape.push_back(i);
  if (args_map["spec_type"] == "lut") {
    if (in_dt != data_type::u8 && in_dt != data_type::s8)
      LOG(ERROR) << "lut specialization only support int8 input_dt.";
    if (out_dt != data_type::bf16) LOG(ERROR) << "lut specialization only support bf16 output_dt now.";
    float in_scale = 1.0, in_zp = 0;
    if (args_map.find("in_scale") != args_map.end()) in_scale = str_to_num<float>(args_map["in_sclae"]);
    if (args_map.find("in_zp") != args_map.end()) in_zp = str_to_num<float>(args_map["in_zp"]);
    postop_attr dequant_attr(in_dt, postop_type::eltwise, postop_alg::dequantize, in_zp, 0, in_scale);
    postop_attrs.push_back(dequant_attr);
    op_attrs["spec_type"] = args_map["spec_type"];
    op_attrs["vec_len"] = std::to_string(shape.back());
    ts_descs = {{shape, in_dt, format_type::undef}, {shape, out_dt, jd::format_type::undef}};
  }

  return {bench_status::success};
}

void softmax_bench::get_true_data() {
  auto op_desc = args.second.op_desc;
  auto rt_data = args.second.rt_data;
  auto src_s8 = reinterpret_cast<int8_t*>(const_cast<void*>(rt_data[0]));
  auto src_u8 = reinterpret_cast<uint8_t*>(const_cast<void*>(rt_data[0]));
  auto dst = reinterpret_cast<bfloat16_t*>(const_cast<void*>(rt_data[1]));
  auto postop_lists = op_desc.apply_postops_list();
  auto src_tensor = op_desc.tensor_descs()[0];
  auto src_dt = src_tensor.dtype();
  auto tensor_shape = src_tensor.shape();
  int row = src_tensor.reduce_rows();
  int col = tensor_shape.back();
  std::vector<float> float_dst_data(row * col, 0);
  for (int i = 0; i < row; i++) {
    // step1. find max
    float max = static_cast<float>(src_u8[0]);
    for (int j = 0; j < col; j++) {
      int src_idx = i * col + j;
      if (src_dt == jd::data_type::s8) {
        max = static_cast<float>(src_s8[src_idx]) > max ? static_cast<float>(src_s8[src_idx]) : max;
      } else {
        max = static_cast<float>(src_u8[src_idx]) > max ? static_cast<float>(src_u8[src_idx]) : max;
      }
    }
    // get e^M
    float one_div_exp_M = 1.0 / get_exp(apply_postop_list(max, postop_lists));
    // step2. compute sum of exp
    float exp_sum = 0;
    for (int j = 0; j < col; j++) {
      float value = 0;
      if (src_dt == jd::data_type::s8) {
        value = apply_postop_list(static_cast<float>(src_s8[i * col + j] - max), postop_lists);
      } else {
        value = apply_postop_list(static_cast<float>(src_u8[i * col + j] - max), postop_lists);
      }
      value = get_exp(value) * one_div_exp_M;
      float_dst_data[i * col + j] = value;
      exp_sum += value;
    }

    float scale = 1 / exp_sum;

    // step3. compute softmax
    for (int j = 0; j < col; j++) dst[i * col + j] = make_bf16(float_dst_data[i * col + j] * scale);
  }
}

bool softmax_bench::check_result() {
  const auto& p = args.first;
  const auto& q = args.second;
  get_true_data();
  auto buf1 = p.rt_data[1];
  auto size1 = p.op_desc.tensor_descs()[1].size();
  auto buf2 = q.rt_data[1];
  auto size2 = q.op_desc.tensor_descs()[1].size();
  bool ans = false;
  auto err_rate = make_bf16(0.1);
  ans = compare_data<bfloat16_t>(buf1, size1, buf2, size2, err_rate);
  return ans;
}

void softmax_bench::gen_case() {
  operator_desc softmax_desc(kernel_kind::softmax, kernel_prop::forward_inference, engine_kind::cpu, ts_descs, op_attrs,
                             postop_attrs);

  int num = get_element_num(softmax_desc);
  void* src = nullptr;
  void* dst = nullptr;
  void* src_ref = nullptr;
  void* dst_ref = nullptr;
  memo_mode MALLOC = memo_mode::MALLOC;
  memo_mode MEMSET = memo_mode::MEMSET;

  auto in_dt = ts_descs[0].dtype();
  auto out_dt = ts_descs[1].dtype();

  src = memo_op(src, num, in_dt, MALLOC);
  dst = memo_op(dst, num, out_dt, MALLOC);
  dst = memo_op(dst, num, out_dt, MEMSET);
  src_ref = memo_op(src_ref, num, in_dt, MALLOC);
  dst_ref = memo_op(dst_ref, num, out_dt, MALLOC);
  dst_ref = memo_op(dst_ref, num, out_dt, MEMSET);

  const unsigned int seed = 667095;
  for (int i = 0; i < num; i++) {
    unsigned int seed_tmp = seed + i;
    float rand_val = rand_r(&seed_tmp) % 256 - 128;
    assign_val(src, in_dt, rand_val, i);
    assign_val(src_ref, in_dt, rand_val, i);
  }

  std::vector<void*> rt_data1;
  std::vector<void*> rt_data2;

  rt_data1.emplace_back(reinterpret_cast<void*>(src));
  rt_data1.emplace_back(reinterpret_cast<void*>(dst));
  rt_data2.emplace_back(reinterpret_cast<void*>(src_ref));
  rt_data2.emplace_back(reinterpret_cast<void*>(dst_ref));

  op_args_t p = {softmax_desc, rt_data1};
  op_args_t q = {softmax_desc, rt_data2};
  args = {p, q};
}

}  // namespace jd
