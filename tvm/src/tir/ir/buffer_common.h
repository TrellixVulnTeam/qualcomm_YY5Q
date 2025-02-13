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
 * \file tir/ir/buffer_common.h
 * \brief Common utils for buffer implementation
 */
#ifndef TVM_TIR_IR_BUFFER_COMMON_H_
#define TVM_TIR_IR_BUFFER_COMMON_H_

#include <iterator>
#include <stack>

namespace tvm {
namespace tir {

using IndexMod = tir::FloorModNode;
using IndexDiv = tir::FloorDivNode;

namespace {
// Split the given expression w.r.t the add operator
inline std::vector<const PrimExpr*> ExprSplitAddition(const PrimExpr& expr) {
  using namespace tir;
  std::vector<const PrimExpr*> ret;
  std::stack<const PrimExpr*> split_buffer;
  split_buffer.push(&expr);
  while (!split_buffer.empty()) {
    const PrimExpr* top_ele = split_buffer.top();
    split_buffer.pop();
    auto expr_add_match = top_ele->as<AddNode>();
    if (expr_add_match) {
      split_buffer.push(&expr_add_match->b);
      split_buffer.push(&expr_add_match->a);
    } else {
      ret.emplace_back(top_ele);
    }
  }
  return ret;
}

// Searches for the following types of expr:
//   mult_expr = (a1 + a2 + ... + aj + c / (k1 * k2 * ... * ki) * k1 * ... * kt-1 ) * kt * ... * ki
//   mod_l_expr = c
//   mod_r_expr = k1 * k2 * ... * ki
// If it can be optimized, returns (true, (a1 + a2 + ... + aj) * kt * ... * ki + c)
// Currently the we will not search the add/mult combinations exhaustively
//   as it will take too much computation.
inline std::pair<bool, PrimExpr> MergeMulModInner(const PrimExpr& mult_expr,
                                                  const PrimExpr& mod_l_expr,
                                                  const PrimExpr& mod_r_expr) {
  using namespace tir;
  const MulNode* mult_ptr = mult_expr.as<MulNode>();
  if (!mult_ptr) return std::make_pair(false, PrimExpr());
  PrimExpr mult_outer = mult_ptr->b;
  const PrimExpr* inner = &(mult_ptr->a);
  // 1. Calculate the outer multiplier
  while (true) {
    mult_ptr = inner->as<MulNode>();
    if (mult_ptr) {
      inner = &(mult_ptr->a);
      mult_outer = mult_ptr->b * mult_outer;
    } else {
      break;
    }
  }
  // 2. Search for the pattern c / (...) * (...) + c % (...)
  // We match the search element with Add, Mul and Div.
  //   If Add is found, we need to continue our search for the rhs
  //   If Mult is found, we will expand the inner multiplication factor
  //   If Div is found, we will go on testing whether lhs matches the lhs of mod expr
  //      and returns the optimization result.
  const PrimExpr* search_ptr = inner;
  PrimExpr mult_inner;  // The inner multiplication factor
  PrimExpr no_opt_sum;  // Sum of the exprs that cannot be optimized
  tir::ExprDeepEqual expr_equal;

  while (true) {
    auto inner_div_ptr = search_ptr->as<IndexDiv>();
    auto inner_mult_ptr = search_ptr->as<MulNode>();
    auto inner_add_ptr = search_ptr->as<AddNode>();
    if (!inner_div_ptr && !inner_mult_ptr && !inner_add_ptr) {
      return std::make_pair(false, PrimExpr());
    } else if (inner_div_ptr) {
      PrimExpr overall_mult = mult_inner.get() ? mult_inner * mult_outer : mult_outer;
      if (expr_equal(overall_mult, inner_div_ptr->b) && expr_equal(overall_mult, mod_r_expr) &&
          expr_equal(inner_div_ptr->a, mod_l_expr)) {
        // Found!
        PrimExpr ret = no_opt_sum.get() ? no_opt_sum * mult_outer + mod_l_expr : mod_l_expr;
        return std::make_pair(true, ret);
      } else {
        return std::make_pair(false, PrimExpr());
      }
    } else if (inner_mult_ptr) {
      mult_inner = mult_inner.get() ? inner_mult_ptr->b * mult_inner : inner_mult_ptr->b;
      search_ptr = &(inner_mult_ptr->a);
    } else if (inner_add_ptr) {
      if (mult_inner.get()) {
        return std::make_pair(false, PrimExpr());
      }
      no_opt_sum = no_opt_sum.get() ? no_opt_sum + inner_add_ptr->a : inner_add_ptr->a;
      search_ptr = &(inner_add_ptr->b);
    } else {
      LOG(FATAL) << "Unexpected search result!";
      break;
    }
  }
  return std::make_pair(false, PrimExpr());
}

// Insert the elements into the corresponding mult_exprs and mod_exprs.
// If the element is found to match Mul, it will be pushed to the mult_exprs.
// If the element it found to match Mod, it will be pused to the mod_exprs.
// Otherwise, the elements will be added to the no_opt_sum variable
inline void MergeMulModInsertElements(const std::vector<const PrimExpr*>& eles,
                                      std::list<PrimExpr>* mult_exprs,
                                      std::list<std::pair<PrimExpr, PrimExpr> >* mod_exprs,
                                      PrimExpr* no_opt_sum, bool* has_mult, bool* has_mod) {
  using namespace tir;
  *has_mult = false;
  *has_mod = false;
  for (const PrimExpr* ele : eles) {
    auto mod_ptr = ele->as<IndexMod>();
    auto mult_ptr = ele->as<MulNode>();
    if (mod_ptr) {
      *has_mod = true;
      mod_exprs->emplace_back(std::make_pair(std::move(mod_ptr->a), std::move(mod_ptr->b)));
    } else if (mult_ptr) {
      *has_mult = true;
      mult_exprs->emplace_back(*ele);
    } else {
      *no_opt_sum = no_opt_sum->get() ? *no_opt_sum + *ele : *ele;
    }
  }
}

// Searches for this types of expr:
//   (a1 + a2 + ... + aj + c / (k1 * k2 * ... * ki) * k1 * ... * kt-1 ) * kt * ... * ki
//   + c % (k1 * k2 * ... * ki)
// and simplifies to (a1 + a2 + ... + aj) * kt * ... * ki + c
// The search will be performed repeatively until no pattern is found.
// Return: a pair with (false, Expr()) if cannot be optimized.
//         a pair with (true, optimized_expr) if can be optimized
inline PrimExpr MergeMulMod(arith::Analyzer* analyzer, const PrimExpr& base) {
  using namespace tir;
  // 1. Prepare the lists.
  // We store two lists, a list that contain all the elements that match Mul and
  //                     a list that contain all the elements that match Mod.
  // The elements in the Mod will be used to match against the elements in Mul.
  // The result will then be split and pushed back to these two lists.
  PrimExpr simplified_base = analyzer->Simplify(base);
  std::vector<const PrimExpr*> eles = ExprSplitAddition(simplified_base);
  std::list<PrimExpr> mult_exprs;
  std::list<std::pair<PrimExpr, PrimExpr> > mod_exprs;
  PrimExpr no_opt_sum;
  bool has_mult;
  bool has_mod;
  MergeMulModInsertElements(eles, &mult_exprs, &mod_exprs, &no_opt_sum, &has_mult, &has_mod);
  bool find_opt = false;
  std::list<std::pair<PrimExpr, PrimExpr> >::iterator search_mod_it = mod_exprs.begin();
  // 2. Exhaustive Search
  while (search_mod_it != mod_exprs.end()) {
    std::list<PrimExpr>::iterator mult_it = mult_exprs.begin();
    bool inner_find_opt = false;
    while (mult_it != mult_exprs.end()) {
      std::pair<bool, PrimExpr> ret =
          MergeMulModInner(*mult_it, search_mod_it->first, search_mod_it->second);
      if (ret.first) {
        inner_find_opt = true;
        auto temp_mod_it = search_mod_it;
        ++search_mod_it;
        mod_exprs.erase(temp_mod_it);
        mult_exprs.erase(mult_it);
        std::vector<const PrimExpr*> ret_eles = ExprSplitAddition(ret.second);
        MergeMulModInsertElements(ret_eles, &mult_exprs, &mod_exprs, &no_opt_sum, &has_mult,
                                  &has_mod);
        if (has_mult) {
          search_mod_it = mod_exprs.begin();
        } else if (has_mod && search_mod_it == mod_exprs.end()) {
          search_mod_it--;
        }
        break;
      } else {
        ++mult_it;
      }
    }
    find_opt = find_opt || inner_find_opt;
    if (!inner_find_opt) {
      ++search_mod_it;
    }
  }
  if (!find_opt) {
    return simplified_base;
  }
  for (std::list<PrimExpr>::iterator it = mult_exprs.begin(); it != mult_exprs.end(); ++it) {
    no_opt_sum = no_opt_sum.get() ? no_opt_sum + *it : *it;
  }
  for (std::list<std::pair<PrimExpr, PrimExpr> >::iterator it = mod_exprs.begin();
       it != mod_exprs.end(); ++it) {
    no_opt_sum = no_opt_sum.get() ? no_opt_sum + indexmod(it->first, it->second)
                                  : indexmod(it->first, it->second);
  }
  return no_opt_sum;
}

// The buffer offset in convention of number of elements of
// original data ignoring number of lanes.
// We also perform optimization to simplify the indexing expression.
inline PrimExpr ElemOffset(const BufferNode* n, Array<PrimExpr> index) {
  PrimExpr base = n->elem_offset;
  arith::Analyzer ana;
  if (n->strides.size() == 0) {
    // Scalar case
    if (n->shape.size() == 0 && index.size() == 1) {
      auto is_int = index[0].as<IntImmNode>();
      ICHECK(is_int && is_int->value == 0);
      base = base + index[0];
    } else {
      ICHECK_EQ(n->shape.size(), index.size());
      if (index.size() > 0) {
        PrimExpr offset = index[0];
        for (size_t i = 1; i < index.size(); ++i) {
          offset = MergeMulMod(&ana, offset * n->shape[i] + index[i]);
        }
        base = base + offset;
      }
    }
  } else {
    ICHECK_EQ(n->strides.size(), index.size());
    if (is_zero(base)) {
      base = MergeMulMod(&ana, index[0] * n->strides[0]);
    } else {
      base = MergeMulMod(&ana, base + index[0] * n->strides[0]);
    }
    for (size_t i = 1; i < index.size(); ++i) {
      base = MergeMulMod(&ana, base + index[i] * n->strides[i]);
    }
  }
  return base;
}

inline PrimExpr BufferOffset(const BufferNode* n, Array<PrimExpr> index, DataType dtype) {
  PrimExpr offset = ElemOffset(n, index);
  if (n->dtype.lanes() != 1) {
    offset = offset * make_const(offset.dtype(), dtype.lanes());
  }
  if (dtype.lanes() != 1) {
    return tir::Ramp(offset, make_const(offset.dtype(), 1), dtype.lanes());
  } else {
    return offset;
  }
}
}  // namespace

}  // namespace tir
}  // namespace tvm
#endif  // TVM_TIR_IR_BUFFER_COMMON_H_
