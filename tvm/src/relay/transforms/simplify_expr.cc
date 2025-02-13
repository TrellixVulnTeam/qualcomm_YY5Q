/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/relay/transforms/simplify_expr.cc
 * \brief A pass for simplifying the Relay expression.
 */

#include <tvm/relay/dataflow_matcher.h>
#include <tvm/relay/expr.h>
#include <tvm/relay/expr_functor.h>
#include <tvm/relay/transform.h>
#include <tvm/support/logging.h>

#include <memory>

#include "../op/tensor/transform.h"
#include "pattern_utils.h"

namespace tvm {
namespace relay {

class SimplifyPattern {
 public:
  virtual Expr callback(const Expr& pre, const Expr& post,
                        const Map<DFPattern, Array<Expr>>& node_map) const = 0;

  DFPattern pattern() const { return pattern_; }

 protected:
  /*! \brief Pattern for rewriting */
  DFPattern pattern_;
};

/*!
 * \brief SimplifyReshape matches the pattern of consecutive reshape or reverse_reshape ops,
 *   and merges into one reshape op.
 */
class SimplifyReshape : public SimplifyPattern {
 public:
  SimplifyReshape() {
    x_ = IsWildcard();
    auto reshape1 = IsOp("reshape") || IsOp("contrib_reverse_reshape");
    auto reshape2 = IsOp("reshape") || IsOp("contrib_reverse_reshape");
    pattern_ = reshape1({reshape2({x_})});
  }

  Expr callback(const Expr& pre, const Expr& post,
                const Map<DFPattern, Array<Expr>>& node_map) const override {
    auto x = node_map[x_][0];
    bool const_shape = true;
    Array<Integer> newshape;
    for (auto dim : Downcast<TensorType>(pre->checked_type())->shape) {
      if (dim.as<IntImmNode>() == nullptr) {
        const_shape = false;
        break;
      }
      newshape.push_back(Downcast<Integer>(dim));
    }
    if (const_shape) {
      return MakeReshape(x, newshape);
    }
    return post;
  }

 private:
  /*! \brief Pattern input */
  DFPattern x_;
};

/*!
 * \brief SimplifyTranspose matches the pattern of consecutive transpose op,
 *   and merges or cancels them.
 */
class SimplifyTranspose : public SimplifyPattern {
 public:
  SimplifyTranspose() {
    x_ = IsWildcard();
    auto trans1 = IsOp("transpose") || IsOp("layout_transform");
    auto trans2 = IsOp("transpose") || IsOp("layout_transform");
    pattern_ = trans1({trans2({x_})});
  }

  Expr callback(const Expr& pre, const Expr& post,
                const Map<DFPattern, Array<Expr>>& node_map) const override {
    auto x = node_map[x_][0];

    Call trans_call = Downcast<Call>(post);

    if (auto layout_trans = FoldRankChangingLayoutTrans(x, trans_call)) {
      if (auto attr = layout_trans.value()->attrs.as<LayoutTransformAttrs>()) {
        // Prune any trivial layout transformation
        if (attr->src_layout == attr->dst_layout) {
          return x;
        }
      }
      return layout_trans.value();
    }

    // Initialize axes
    int ndim = Downcast<TensorType>(pre->checked_type())->shape.size();
    Array<Integer> axes;
    for (int i = 0; i < ndim; ++i) {
      axes.push_back(i);
    }

    // Collect axes changes from the matched pattern, including two consecutive transposes.
    std::vector<std::vector<int>> interm_axes;
    interm_axes.push_back(GetTransposeAxisOrder(trans_call, ndim));
    trans_call = Downcast<Call>(trans_call->args[0]);
    interm_axes.push_back(GetTransposeAxisOrder(trans_call, ndim));

    // Calculate the final axes in reverse order (from root to output)
    auto it = interm_axes.rbegin();
    while (it != interm_axes.rend()) {
      auto interm = *it;

      Array<Integer> new_axes;
      for (int i = 0; i < ndim; ++i) {
        new_axes.push_back(axes[interm[i]]);
      }
      axes = new_axes;
      it++;
    }

    // Check if the transpose is still required
    bool need_transpose = false;
    for (int i = 0; i < ndim; ++i) {
      if (axes[i] != i) {
        need_transpose = true;
        break;
      }
    }

    if (need_transpose) {
      return MakeTranspose(x, axes);
    }
    return x;
  }

  String PermuteLayout(const String& layout, std::vector<int> axes) const {
    std::string new_layout{};
    std::string old_layout{layout};
    for (auto axis : axes) {
      new_layout += old_layout[axis];
    }
    return String(new_layout);
  }

  struct RankChangingLayoutDescriptor {
    Layout src_layout;
    Layout dst_layout;
    // Either a rank changing layout transform or a transpose
    Call other_transform;
  };

  std::unique_ptr<RankChangingLayoutDescriptor> GetRankChangeDescriptor(const Call& call) const {
    std::unique_ptr<RankChangingLayoutDescriptor> desc{nullptr};
    if (auto attr = call->attrs.as<LayoutTransformAttrs>()) {
      if (attr->src_layout.length() != attr->dst_layout.length()) {
        desc = std::make_unique<RankChangingLayoutDescriptor>();
        desc->src_layout = Layout(attr->src_layout);
        desc->dst_layout = Layout(attr->dst_layout);
        desc->other_transform = Downcast<Call>(call->args[0]);
      }
    }
    if (auto attr = Downcast<Call>(call->args[0])->attrs.as<LayoutTransformAttrs>()) {
      if (attr->src_layout.length() != attr->dst_layout.length()) {
        if (!desc) {
          desc = std::make_unique<RankChangingLayoutDescriptor>();
          desc->src_layout = Layout(attr->src_layout);
          desc->dst_layout = Layout(attr->dst_layout);
          desc->other_transform = call;
        } else {
          ICHECK(desc->src_layout->name == attr->dst_layout)
              << "Back-to-back layout transforms must have the same intermediate layout: "
              << desc->src_layout->name << " != " << attr->dst_layout;
          desc->src_layout = Layout(attr->src_layout);
        }
      }
    }
    return desc;
  }

  /*
   * \brief Fuse call and it's argument into a single layout_transform operator
   * when either call or it's argument is a rang changing layout_transform, e.g.,
   *
   *  Simplify
   *
   *  [N, H, W, C] -> Transpose -> [N, C, H, W] -> LayoutTrans -> [N, C, H, W, 4c]
   *
   *  to,
   *
   *  [N, H, W, C] -> LayoutTrans -> [N, C, H, W, 4c].
   *
   * \param The input expression to the matched pattern
   * \param The pattern root; the second of two consecutive Transpose/LayoutTransform ops
   */
  Optional<Call> FoldRankChangingLayoutTrans(const Expr& data, const Call& call) const {
    auto desc = GetRankChangeDescriptor(call);
    if (desc == nullptr) {
      return Optional<Call>{nullptr};
    }

    Optional<Expr> output_layout_trans;
    if (desc->src_layout->axes.size() < desc->dst_layout->axes.size()) {
      auto axes = GetTransposeAxisOrder(desc->other_transform, desc->src_layout->axes.size());
      std::vector<int> inverse(axes.size());
      for (size_t i = 0; i < axes.size(); i++) {
        inverse[axes[i]] = i;
      }
      String new_layout = PermuteLayout(std::string(desc->src_layout->name), inverse);
      output_layout_trans = MakeLayoutTransform(data, new_layout, desc->dst_layout->name);
    } else if (desc->src_layout->axes.size() > desc->dst_layout->axes.size()) {
      auto axes = GetTransposeAxisOrder(desc->other_transform, desc->dst_layout->axes.size());
      String new_layout = PermuteLayout(std::string(desc->dst_layout->name), axes);
      output_layout_trans = MakeLayoutTransform(data, desc->src_layout->name, new_layout);
    } else if (desc->other_transform->attrs.as<LayoutTransformAttrs>()) {
      // Fuse two consecutive layout transforms
      output_layout_trans =
          MakeLayoutTransform(data, desc->src_layout->name, desc->dst_layout->name);
    }
    return Downcast<Call>(output_layout_trans);
  }

  std::vector<int> GetTransposeAxisOrder(const Call& call, int ndim) const {
    std::vector<int> attr_axes;
    if (auto attr = call->attrs.as<TransposeAttrs>()) {
      if (attr->axes.defined()) {
        for (int i = 0; i < ndim; ++i) {
          int64_t axis = attr->axes[i];
          axis += (axis < 0) ? ndim : 0;
          attr_axes.push_back(axis);
        }
      } else {
        // Empty axes means reverse
        for (int i = ndim - 1; i >= 0; --i) {
          attr_axes.push_back(i);
        }
      }
    } else if (auto attr = call->attrs.as<LayoutTransformAttrs>()) {
      Layout src_layout(attr->src_layout);
      Layout dst_layout(attr->dst_layout);
      for (int i = 0; i < ndim; ++i) {
        attr_axes.push_back(src_layout.IndexOf(dst_layout[i]));
      }
    } else {
      CHECK(false) << "Expected transpose or layout_transform, but got "
                   << Downcast<Op>(call->op)->name;
    }
    return std::move(attr_axes);
  }

 private:
  /*! \brief Pattern input */
  DFPattern x_;
};

/*!
 * \brief FullArgwhere finds full followed by argwhere and turns it into an Arange op
 */
class FullElementwise : public SimplifyPattern {
 public:
  FullElementwise() {
    x_ = IsWildcard();
    data_ = IsWildcard();
    value_ = IsConstant();

    full_ = IsOp("full")({value_}) || IsOp("full_like")({data_, value_});
    ones_ = IsOp("ones")({}) || IsOp("ones_like")({data_});
    zeros_ = IsOp("zeros")({}) || IsOp("zeros_like")({data_});

    Map<String, ObjectRef> attrs;
    attrs.Set("TOpPattern", Integer(static_cast<int>(kBroadcast)));
    DFPattern op = IsWildcard().HasAttr(attrs);
    DFPattern full = full_ || ones_ || zeros_;
    pattern_ = op({full, x_}) || op({x_, full});
  }

  Expr callback(const Expr& pre, const Expr& post,
                const Map<DFPattern, Array<Expr>>& node_map) const override {
    const CallNode* call = pre.as<CallNode>();
    ICHECK(call);
    Type pre_type = pre->checked_type_;
    ICHECK(pre_type.as<TensorTypeNode>());
    auto dtype = pre_type.as<TensorTypeNode>()->dtype;
    auto x = node_map[x_][0];
    bool is_left = post.as<CallNode>()->args[1] == x;
    Type x_type;
    if (is_left) {
      x_type = call->args[1]->checked_type_;
    } else {
      x_type = call->args[0]->checked_type_;
    }

    if (StructuralEqual()(x_type, pre_type)) {
      Expr value;
      if (node_map.count(full_)) {
        value = node_map[value_][0];
        ICHECK(IsConstScalar(value));
      } else if (node_map.count(ones_)) {
        value = MakeConstantScalar(dtype, 1);
      } else if (node_map.count(zeros_)) {
        value = MakeConstantScalar(dtype, 0);
      } else {
        ICHECK(false) << "Didn't find a full op while matching full + elementwise";
      }
      if (is_left) {
        return Call(call->op, {value, x}, call->attrs, call->type_args, call->span);
      } else {
        return Call(call->op, {x, value}, call->attrs, call->type_args, call->span);
      }
    }
    return post;
  }

 private:
  /*! \brief binary argument */
  DFPattern x_;
  /*! \brief data ops get shape from */
  DFPattern data_;
  /*! \brief constant input */
  DFPattern value_;
  /*! \brief full op */
  DFPattern full_;
  /*! \brief ones op */
  DFPattern ones_;
  /*! \brief zeros op */
  DFPattern zeros_;
};

/*!
 * \brief ExprSimplifier simplifies the Relay expression.
 */
class ExprSimplifier {
 public:
  explicit ExprSimplifier(IRModule mod) : mod_(mod) {
    CreateCallback(SimplifyReshape());
    CreateCallback(SimplifyTranspose());
    CreateCallback(FullElementwise());
  }
  template <typename T>
  void CreateCallback(const T& pattern) {
    auto func = [pattern](TVMArgs args, TVMRetValue* rv) {
      Expr pre = args[0];
      Expr post = args[1];
      Map<DFPattern, Array<Expr>> node_map = args[2];
      *rv = pattern.callback(pre, post, node_map);
    };
    callbacks_.push_back(DFPatternCallback(pattern.pattern(), PackedFunc(func), true));
  }

  Expr Simplify(const Expr& expr) { return RewritePatterns(callbacks_, expr, mod_); }

 private:
  IRModule mod_;
  /*! \brief Callbacks for expr simplification */
  Array<DFPatternCallback> callbacks_;
};

Expr SimplifyExpr(const Expr& expr, const IRModule& mod) {
  return ExprSimplifier(mod).Simplify(expr);
}

namespace transform {

Pass SimplifyExpr() {
  runtime::TypedPackedFunc<Function(Function, IRModule, PassContext)> pass_func =
      [=](Function f, IRModule m, PassContext pc) {
        return Downcast<Function>(SimplifyExpr(f, m));
      };
  return CreateFunctionPass(pass_func, 0, "SimplifyExpr", {"InferType"});
}

TVM_REGISTER_GLOBAL("relay._transform.SimplifyExpr").set_body_typed(SimplifyExpr);

}  // namespace transform

}  // namespace relay
}  // namespace tvm
