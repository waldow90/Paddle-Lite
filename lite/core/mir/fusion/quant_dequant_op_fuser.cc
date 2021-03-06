// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
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

#include "lite/core/mir/fusion/quant_dequant_op_fuser.h"
#include <memory>
#include <unordered_set>
#include <vector>
#include "lite/utils/string.h"

namespace paddle {
namespace lite {
namespace mir {
namespace fusion {

void DeleteQuantOpFuser::BuildPattern() {
  auto* input_scale_node = VarNode("input_scale_node")
                               ->assert_is_op_input(quant_op_type_, "InScale");
  auto* input_act_node =
      VarNode("input_act_node")->assert_is_op_input(quant_op_type_, "X");
  auto* quant_node =
      OpNode("quant_node", quant_op_type_)->assert_is_op(quant_op_type_);
  auto* output_scale_node =
      VarNode("output_scale_node")
          ->assert_is_op_output(quant_op_type_, "OutScale");
  auto* output_act_node =
      VarNode("output_act_node")->assert_is_op_output(quant_op_type_, "Out");

  quant_node->LinksFrom({input_scale_node, input_act_node});
  output_scale_node->LinksFrom({quant_node});
  output_act_node->LinksFrom({quant_node});
  VLOG(4) << "DeleteQuantOpFuser BuildPattern quant_op_type:" << quant_op_type_;
}

void DeleteQuantOpFuser::InsertNewNode(SSAGraph* graph,
                                       const key2nodes_t& matched) {
  auto* input_scale_node = matched.at("input_scale_node");
  auto* input_act_node = matched.at("input_act_node");
  auto* quant_node = matched.at("quant_node");
  auto* output_scale_node = matched.at("output_scale_node");
  auto* output_act_node = matched.at("output_act_node");

  // obtain values, save values and relink node
  int bit_length = quant_node->stmt()->op_info()->GetAttr<int>("bit_length");
  int range = ((1 << (bit_length - 1)) - 1);
  auto* scope = quant_node->stmt()->op()->scope();
  auto* scale_tensor = scope->FindVar(output_scale_node->arg()->name)
                           ->GetMutable<lite::Tensor>();
  float scale_value = scale_tensor->data<float>()[0] / range;

  auto outlinks = output_act_node->outlinks;
  for (auto* quantized_node : outlinks) {
    auto* op_desc = quantized_node->stmt()->mutable_op_info();
    op_desc->SetAttr<int>("bit_length", bit_length);
    op_desc->SetAttr<float>("input_scale", scale_value);
    IR_NODE_LINK_TO(input_act_node, quantized_node)
  }

  // delete nodes and edges
  std::unordered_set<const Node*> nodes2rm = {
      input_scale_node, quant_node, output_scale_node, output_act_node};
  GraphSafeRemoveNodes(graph, nodes2rm);
}

cpp::OpDesc DeleteQuantOpFuser::GenOpDesc(const key2nodes_t& matched) {
  cpp::OpDesc op_desc;
  return op_desc;
}

void DequantOpFuser::BuildPattern() {
  std::string weight_name = "";
  if (quantized_op_type_ == "conv2d" ||
      quantized_op_type_ == "depthwise_conv2d") {
    weight_name = "Filter";
  } else {
    weight_name = "Y";
  }

  auto* quantized_op_input = VarNode("quantized_op_input")
                                 ->assert_is_op_input(quantized_op_type_)
                                 ->AsInput();
  auto* quantized_op_weight =
      VarNode("quantized_op_weight")
          ->assert_is_op_input(quantized_op_type_, weight_name)
          ->AsInput();
  auto* quantized_op = OpNode("quantized_op", quantized_op_type_)
                           ->assert_is_op(quantized_op_type_)
                           ->AsIntermediate();
  auto* quantized_op_out =
      VarNode("quantized_op_out")
          ->assert_is_op_output(quantized_op_type_)
          ->assert_is_op_input("fake_dequantize_max_abs", "X")
          ->AsIntermediate();
  auto* dequant_op = OpNode("dequant_op", "fake_dequantize_max_abs")
                         ->assert_is_op("fake_dequantize_max_abs")
                         ->AsIntermediate();
  auto* dequant_op_out =
      VarNode("dequant_op_out")
          ->assert_is_op_output("fake_dequantize_max_abs", "Out")
          ->AsOutput();

  quantized_op->LinksFrom({quantized_op_input, quantized_op_weight});
  quantized_op_out->LinksFrom({quantized_op});
  dequant_op->LinksFrom({quantized_op_out});
  dequant_op_out->LinksFrom({dequant_op});

  VLOG(4) << "DeQuantOpFuser BuildPattern op_type:" << quantized_op_type_;
}

void DequantOpFuser::InsertNewNode(SSAGraph* graph,
                                   const key2nodes_t& matched) {
  auto* quantized_op_input = matched.at("quantized_op_input");
  auto* quantized_op_weight = matched.at("quantized_op_weight");
  auto* quantized_op = matched.at("quantized_op");
  auto* dequant_op = matched.at("dequant_op");
  auto* dequant_op_out = matched.at("dequant_op_out");

  // obtain input_scale and weight_scale
  auto* scope = quantized_op->stmt()->op()->scope();
  auto& valid_places = quantized_op->stmt()->op()->valid_places();
  int bit_length = quantized_op->stmt()->op_info()->GetAttr<int>("bit_length");
  int range = ((1 << (bit_length - 1)) - 1);
  float input_scale =
      quantized_op->stmt()->op_info()->GetAttr<float>("input_scale");
  float max_range = dequant_op->stmt()->op_info()->GetAttr<float>("max_range");
  float whole_weight_scale =
      static_cast<float>(range * range) / max_range / range;
  // max_range = range * range / max(abs(weight))
  // weight_scale = range * range / (range * range / max(abs(weight))) / range
  //              = max(abs(weight)) / range

  // set op desc
  cpp::OpDesc op_desc = *quantized_op->stmt()->op_info();
  auto quantized_weight_var_name = quantized_op_weight->arg()->name;
  auto quantized_weight_t =
      scope->FindVar(quantized_weight_var_name)->GetMutable<lite::Tensor>();
  std::vector<float> weight_scale;
  int weight_scale_size;
  if (quantized_op_type_ == "conv2d" ||
      quantized_op_type_ == "depthwise_conv2d") {
    op_desc.SetInput("Input", {quantized_op_input->arg()->name});
    op_desc.SetOutput("Output", {dequant_op_out->arg()->name});
    // Conv weight shape: Cout * Cin * kh * hw, the weight_scale_size should
    // be Cout.
    weight_scale_size = quantized_weight_t->dims()[0];
  } else if (quantized_op_type_ == "mul") {
    op_desc.SetInput("X", {quantized_op_input->arg()->name});
    op_desc.SetOutput("Out", {dequant_op_out->arg()->name});
    // Fc weight: Cin * Cout, the weight_scale_size should be Cout.
    weight_scale_size = quantized_weight_t->dims()[1];
  }
  for (int i = 0; i < weight_scale_size; i++) {
    weight_scale.push_back(whole_weight_scale);
  }
  op_desc.SetAttr("enable_int8", true);
  op_desc.SetAttr("input_scale", input_scale);
  op_desc.SetAttr("weight_scale", weight_scale);

  // change the weight from the float type to int8 type.
  Tensor temp_tensor;
  temp_tensor.CopyDataFrom(*quantized_weight_t);
  float* temp_data = temp_tensor.mutable_data<float>();
  size_t weight_num = quantized_weight_t->data_size();
  int8_t* quantized_weight_data = quantized_weight_t->mutable_data<int8_t>();
  for (size_t i = 0; i < weight_num; i++) {
    quantized_weight_data[i] = static_cast<int8_t>(temp_data[i]);
  }
  quantized_weight_t->set_persistable(true);
  quantized_weight_t->set_precision(PRECISION(kInt8));

  // new op and relink nodes
  auto new_quantized_op = LiteOpRegistry::Global().Create(quantized_op_type_);
  new_quantized_op->Attach(op_desc, scope);
  auto* new_quantized_op_node =
      graph->GraphCreateInstructNode(new_quantized_op, valid_places);
  IR_NODE_LINK_TO(quantized_op_input, new_quantized_op_node);
  IR_NODE_LINK_TO(quantized_op_weight, new_quantized_op_node);
  IR_NODE_LINK_TO(new_quantized_op_node, dequant_op_out);
}

cpp::OpDesc DequantOpFuser::GenOpDesc(const key2nodes_t& matched) {
  cpp::OpDesc op_desc;
  return op_desc;
}

void ChannelWiseDequantOpFuser::BuildPattern() {
  std::string dequant_op_type = "fake_channel_wise_dequantize_max_abs";
  auto* quantized_op_input = VarNode("quantized_op_input")
                                 ->assert_is_op_input(quantized_op_type_)
                                 ->AsInput();
  auto* quantized_op_weight =
      VarNode("quantized_op_weight")
          ->assert_is_op_input(quantized_op_type_, "Filter")
          ->AsInput();
  auto* quantized_op = OpNode("quantized_op", quantized_op_type_)
                           ->assert_is_op(quantized_op_type_)
                           ->AsIntermediate();
  auto* quantized_op_out = VarNode("quantized_op_out")
                               ->assert_is_op_output(quantized_op_type_)
                               ->assert_is_op_input(dequant_op_type, "X")
                               ->AsIntermediate();
  auto* dequant_op_channel_scale = VarNode("dequant_op_channel_scale")
                                       ->assert_is_op_input(dequant_op_type)
                                       ->AsIntermediate();
  auto* dequant_op = OpNode("dequant_op", dequant_op_type)
                         ->assert_is_op(dequant_op_type)
                         ->AsIntermediate();
  auto* dequant_op_out = VarNode("dequant_op_out")
                             ->assert_is_op_output(dequant_op_type, "Out")
                             ->AsOutput();

  quantized_op->LinksFrom({quantized_op_input, quantized_op_weight});
  quantized_op_out->LinksFrom({quantized_op});
  dequant_op->LinksFrom({quantized_op_out, dequant_op_channel_scale});
  dequant_op_out->LinksFrom({dequant_op});

  VLOG(4) << "ChannelWiseDequantOpFuser BuildPattern op_type:"
          << quantized_op_type_;
}

void ChannelWiseDequantOpFuser::InsertNewNode(SSAGraph* graph,
                                              const key2nodes_t& matched) {
  auto* quantized_op_input = matched.at("quantized_op_input");
  auto* quantized_op_weight = matched.at("quantized_op_weight");
  auto* quantized_op = matched.at("quantized_op");
  auto* dequant_op_channel_scale = matched.at("dequant_op_channel_scale");
  auto* dequant_op = matched.at("dequant_op");
  auto* dequant_op_out = matched.at("dequant_op_out");

  // obtain input_scale and weight_scale
  auto* scope = quantized_op->stmt()->op()->scope();
  auto& valid_places = quantized_op->stmt()->op()->valid_places();
  float input_scale =
      quantized_op->stmt()->op_info()->GetAttr<float>("input_scale");

  std::vector<float> weight_scale;
  std::vector<int> quant_bits =
      dequant_op->stmt()->op_info()->GetAttr<std::vector<int>>("quant_bits");
  int weight_bit_length = quant_bits[0];
  int range = ((1 << (weight_bit_length - 1)) - 1);
  auto channel_scale_name = dequant_op_channel_scale->arg()->name;
  auto channel_scale_tensor =
      scope->FindVar(channel_scale_name)->GetMutable<lite::Tensor>();
  auto* channel_scale_data = channel_scale_tensor->data<float>();
  for (int i = 0; i < channel_scale_tensor->data_size(); i++) {
    weight_scale.push_back(channel_scale_data[i] / range);
  }

  // set op desc
  cpp::OpDesc op_desc = *quantized_op->stmt()->op_info();
  op_desc.SetInput("Input", {quantized_op_input->arg()->name});
  op_desc.SetOutput("Output", {dequant_op_out->arg()->name});

  op_desc.SetAttr("enable_int8", true);
  op_desc.SetAttr("input_scale", input_scale);
  op_desc.SetAttr("weight_scale", weight_scale);

  // change the weight from the float type to int8 type.
  auto quantized_weight_var_name = quantized_op_weight->arg()->name;
  auto quantized_weight_t =
      scope->FindVar(quantized_weight_var_name)->GetMutable<lite::Tensor>();
  Tensor temp_tensor;
  temp_tensor.CopyDataFrom(*quantized_weight_t);
  float* temp_data = temp_tensor.mutable_data<float>();
  int8_t* quantized_weight_data = quantized_weight_t->mutable_data<int8_t>();
  for (size_t i = 0; i < quantized_weight_t->data_size(); i++) {
    quantized_weight_data[i] = static_cast<int8_t>(temp_data[i]);
  }
  quantized_weight_t->set_persistable(true);
  quantized_weight_t->set_precision(PRECISION(kInt8));

  // new op and relink nodes
  auto new_quantized_op = LiteOpRegistry::Global().Create(quantized_op_type_);
  new_quantized_op->Attach(op_desc, scope);
  auto* new_quantized_op_node =
      graph->GraphCreateInstructNode(new_quantized_op, valid_places);
  IR_NODE_LINK_TO(quantized_op_input, new_quantized_op_node);
  IR_NODE_LINK_TO(quantized_op_weight, new_quantized_op_node);
  IR_NODE_LINK_TO(new_quantized_op_node, dequant_op_out);
}

cpp::OpDesc ChannelWiseDequantOpFuser::GenOpDesc(const key2nodes_t& matched) {
  cpp::OpDesc op_desc;
  return op_desc;
}

void DeleteQuantDequantOpFuser::BuildPattern() {
  std::string quant_dequant_op_type =
      "fake_quantize_dequantize_moving_average_abs_max";
  if (quantized_op_type_ == "pool2d") {
    auto* input_scale_node =
        VarNode("input_scale_node")
            ->assert_is_op_input(quant_dequant_op_type, "InScale");
    auto* input_act_node = VarNode("input_act_node")
                               ->assert_is_op_input(quant_dequant_op_type, "X");
    auto* quant_dequant_node =
        OpNode("quant_dequant_node", quant_dequant_op_type)
            ->assert_is_op(quant_dequant_op_type);
    auto* output_scale_node =
        VarNode("output_scale_node")
            ->assert_is_op_output(quant_dequant_op_type, "OutScale");
    auto* output_act_node =
        VarNode("output_act_node")
            ->assert_is_op_output(quant_dequant_op_type, "Out");
    auto* quantized_node = OpNode("quantized_node", quantized_op_type_)
                               ->assert_is_op(quantized_op_type_);

    quant_dequant_node->LinksFrom({input_scale_node, input_act_node});
    output_scale_node->LinksFrom({quant_dequant_node});
    output_act_node->LinksFrom({quant_dequant_node});
    quantized_node->LinksFrom({output_act_node});
  } else if (quantized_op_type_ == "elementwise_add") {
    auto* input_scale_left_node =
        VarNode("input_scale_left_node")
            ->assert_is_op_input(quant_dequant_op_type, "InScale");
    auto* input_act_left_node =
        VarNode("input_act_left_node")
            ->assert_is_op_input(quant_dequant_op_type, "X");
    auto* quant_dequant_left_node =
        OpNode("quant_dequant_left_node", quant_dequant_op_type)
            ->assert_is_op(quant_dequant_op_type);
    auto* output_scale_left_node =
        VarNode("output_scale_left_node")
            ->assert_is_op_output(quant_dequant_op_type, "OutScale");
    auto* output_act_left_node =
        VarNode("output_act_left_node")
            ->assert_is_op_output(quant_dequant_op_type, "Out")
            ->assert_is_op_input(quantized_op_type_, "X");
    quant_dequant_left_node->LinksFrom(
        {input_scale_left_node, input_act_left_node});
    output_scale_left_node->LinksFrom({quant_dequant_left_node});
    output_act_left_node->LinksFrom({quant_dequant_left_node});

    auto* input_scale_right_node =
        VarNode("input_scale_right_node")
            ->assert_is_op_input(quant_dequant_op_type, "InScale");
    auto* input_act_right_node =
        VarNode("input_act_right_node")
            ->assert_is_op_input(quant_dequant_op_type, "X");
    auto* quant_dequant_right_node =
        OpNode("quant_dequant_right_node", quant_dequant_op_type)
            ->assert_is_op(quant_dequant_op_type);
    auto* output_scale_right_node =
        VarNode("output_scale_right_node")
            ->assert_is_op_output(quant_dequant_op_type, "OutScale");
    auto* output_act_right_node =
        VarNode("output_act_right_node")
            ->assert_is_op_output(quant_dequant_op_type, "Out")
            ->assert_is_op_input(quantized_op_type_, "Y");
    quant_dequant_right_node->LinksFrom(
        {input_scale_right_node, input_act_right_node});
    output_scale_right_node->LinksFrom({quant_dequant_right_node});
    output_act_right_node->LinksFrom({quant_dequant_right_node});

    auto* quantized_node = OpNode("quantized_node", quantized_op_type_)
                               ->assert_is_op(quantized_op_type_);
    quantized_node->LinksFrom({output_act_left_node, output_act_right_node});
  } else {
    LOG(FATAL) << "No support quantized_op_type:" << quantized_op_type_;
  }
  VLOG(4) << "DeleteQuantDequantOpFuser BuildPattern op_type:"
          << quantized_op_type_;
}

void DeleteQuantDequantOpFuser::InsertNewNode(SSAGraph* graph,
                                              const key2nodes_t& matched) {
  if (quantized_op_type_ == "pool2d") {
    auto* input_scale_node = matched.at("input_scale_node");
    auto* input_act_node = matched.at("input_act_node");
    auto* quant_dequant_node = matched.at("quant_dequant_node");
    auto* output_scale_node = matched.at("output_scale_node");
    auto* output_act_node = matched.at("output_act_node");
    auto* quantized_node = matched.at("quantized_node");

    // obtain values, save values and relink node
    int bit_length =
        quant_dequant_node->stmt()->op_info()->GetAttr<int>("bit_length");
    int range = ((1 << (bit_length - 1)) - 1);
    auto* scope = quant_dequant_node->stmt()->op()->scope();
    auto* scale_tensor = scope->FindVar(output_scale_node->arg()->name)
                             ->GetMutable<lite::Tensor>();
    float scale_value = scale_tensor->data<float>()[0] / range;

    auto* op_desc = quantized_node->stmt()->mutable_op_info();
    op_desc->SetAttr<int>("bit_length", bit_length);
    op_desc->SetAttr<float>("input_scale", scale_value);
    op_desc->SetInput("X", {input_act_node->arg()->name});
    IR_NODE_LINK_TO(input_act_node, quantized_node)

    // delete nodes and edges
    std::unordered_set<const Node*> nodes2rm = {input_scale_node,
                                                quant_dequant_node,
                                                output_scale_node,
                                                output_act_node};
    GraphSafeRemoveNodes(graph, nodes2rm);
  } else if (quantized_op_type_ == "elementwise_add") {
    auto* input_scale_left_node = matched.at("input_scale_left_node");
    auto* input_act_left_node = matched.at("input_act_left_node");
    auto* quant_dequant_left_node = matched.at("quant_dequant_left_node");
    auto* output_scale_left_node = matched.at("output_scale_left_node");
    auto* output_act_left_node = matched.at("output_act_left_node");

    auto* input_scale_right_node = matched.at("input_scale_right_node");
    auto* input_act_right_node = matched.at("input_act_right_node");
    auto* quant_dequant_right_node = matched.at("quant_dequant_right_node");
    auto* output_scale_right_node = matched.at("output_scale_right_node");
    auto* output_act_right_node = matched.at("output_act_right_node");

    auto* quantized_node = matched.at("quantized_node");

    // obtain values, save values and relink node
    int bit_length =
        quant_dequant_left_node->stmt()->op_info()->GetAttr<int>("bit_length");
    int range = ((1 << (bit_length - 1)) - 1);
    auto* scope = quant_dequant_left_node->stmt()->op()->scope();
    auto* left_scale_tensor =
        scope->FindVar(output_scale_left_node->arg()->name)
            ->GetMutable<lite::Tensor>();
    float left_scale_value = left_scale_tensor->data<float>()[0] / range;
    auto* right_scale_tensor =
        scope->FindVar(output_scale_right_node->arg()->name)
            ->GetMutable<lite::Tensor>();
    float right_scale_value = right_scale_tensor->data<float>()[0] / range;

    auto* op_desc = quantized_node->stmt()->mutable_op_info();
    op_desc->SetAttr<int>("bit_length", bit_length);
    op_desc->SetAttr<float>("x_input_scale", left_scale_value);
    op_desc->SetAttr<float>("y_input_scale", right_scale_value);
    op_desc->SetInput("X", {input_act_left_node->arg()->name});
    op_desc->SetInput("Y", {input_act_right_node->arg()->name});
    IR_NODE_LINK_TO(input_act_left_node, quantized_node)
    IR_NODE_LINK_TO(input_act_right_node, quantized_node)

    // delete nodes and edges
    std::unordered_set<const Node*> nodes2rm = {input_scale_left_node,
                                                quant_dequant_left_node,
                                                output_scale_left_node,
                                                output_act_left_node,
                                                input_scale_right_node,
                                                quant_dequant_right_node,
                                                output_scale_right_node,
                                                output_act_right_node};
    GraphSafeRemoveNodes(graph, nodes2rm);
  } else {
    LOG(FATAL) << "No support quantized_op_type:" << quantized_op_type_;
  }
}

cpp::OpDesc DeleteQuantDequantOpFuser::GenOpDesc(const key2nodes_t& matched) {
  cpp::OpDesc op_desc;
  return op_desc;
}

}  // namespace fusion
}  // namespace mir
}  // namespace lite
}  // namespace paddle
