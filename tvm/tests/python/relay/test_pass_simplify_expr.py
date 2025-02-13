# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
import tvm
from tvm import relay
from tvm.relay import transform
from tvm.relay.testing import run_opt_pass

import numpy as np


def test_simplify_reshape():
    def before():
        x = relay.var("x", shape=(1, 16, 16, 16), dtype="float32")
        w = relay.var("w", shape=(32, 16, 3, 3), dtype="float32")
        y = relay.nn.conv2d(x, w, padding=(1, 1))
        y = relay.reshape(y, newshape=(1, 16, -1))
        y = relay.reshape(y, newshape=(4, 8, -1, 16))
        y = relay.reverse_reshape(y, newshape=(32, 0, -1))
        return relay.Function([x, w], y)

    def expected():
        x = relay.var("x", shape=(1, 16, 16, 16), dtype="float32")
        w = relay.var("w", shape=(32, 16, 3, 3), dtype="float32")
        y = relay.nn.conv2d(x, w, padding=(1, 1))
        y = relay.reshape(y, newshape=(32, 16, 16))
        return relay.Function([x, w], y)

    def symbolic():
        b = tvm.te.size_var("b")
        x = relay.var("x", shape=(b, 16, 16, 16), dtype="float32")
        w = relay.var("w", shape=(32, 16, 3, 3), dtype="float32")
        y = relay.nn.conv2d(x, w, padding=(1, 1))
        y = relay.reshape(y, newshape=(1, 16, -1))
        y = relay.reshape(y, newshape=(4, 8, -1, 16))
        y = relay.reverse_reshape(y, newshape=(32, 0, -1))
        return relay.Function([x, w], y)

    z = before()
    zz = run_opt_pass(z, transform.SimplifyExpr())
    after = run_opt_pass(expected(), transform.InferType())
    assert tvm.ir.structural_equal(zz, after)

    z = symbolic()
    zz = run_opt_pass(z, transform.SimplifyExpr())
    after = run_opt_pass(symbolic(), transform.InferType())
    assert tvm.ir.structural_equal(zz, after)


def test_simplify_transpose():
    # Test a series of transpose and layout_transform ops
    def before1():
        x = relay.var("x", shape=(1, 3, 224, 224), dtype="float32")  # NCHW
        y = relay.transpose(x, axes=[0, 2, 3, 1])  # To NHWC
        y = relay.layout_transform(y, "NHWC", "HWCN")  # To HWCN
        y = relay.transpose(y, axes=[3, 0, 1, 2])  # To NHWC
        return relay.Function([x], y)

    def expected1():
        x = relay.var("x", shape=(1, 3, 224, 224), dtype="float32")  # NCHW
        y = relay.transpose(x, axes=[0, 2, 3, 1])  # To NHWC
        return relay.Function([x], y)

    # Test that all transpose ops can be cancelled
    def before2():
        x = relay.var("x", shape=(1, 3, 224, 224), dtype="float32")  # NCHW
        y = relay.nn.relu(x)
        y = relay.transpose(y, axes=[0, 2, 3, 1])  # To NHWC
        y = relay.transpose(y, axes=[1, 2, 3, 0])  # To HWCN
        y = relay.transpose(y, axes=[3, 2, 0, 1])  # To NCHW
        return relay.Function([x], y)

    def expected2():
        x = relay.var("x", shape=(1, 3, 224, 224), dtype="float32")  # NCHW
        y = relay.nn.relu(x)
        return relay.Function([x], y)

    # Test default axis (reverse) and negative axis
    def before3():
        x = relay.var("x", shape=(1, 3, 224, 224), dtype="float32")  # NCHW
        y = relay.nn.relu(x)
        y = relay.transpose(y)  # Reverse
        y = relay.transpose(y)  # Reverse
        y = relay.transpose(y, axes=[0, 2, -1, 1])
        y = relay.transpose(y)  # Reverse
        y = relay.transpose(y)  # Reverse
        return relay.Function([x], y)

    def expected3():
        x = relay.var("x", shape=(1, 3, 224, 224), dtype="float32")  # NCHW
        y = relay.nn.relu(x)
        y = relay.transpose(y, axes=[0, 2, 3, 1])
        return relay.Function([x], y)

    # Test a series of transpose and rank changing layout_transform
    def before4():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.transpose(x, axes=[0, 3, 1, 2])  # To NCHW
        y = relay.layout_transform(y, "NCHW", "NCHW4c")  # To NCHW4c
        y = relay.nn.relu(y)
        y = relay.layout_transform(y, "NCHW4c", "NCHW")  # To NCHW
        y = relay.transpose(y, axes=[0, 2, 3, 1])  # To NHWC
        return relay.Function([x], y)

    def expected4():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.layout_transform(x, "NHWC", "NCHW4c")  # To NCHW4c
        y = relay.nn.relu(y)
        y = relay.layout_transform(y, "NCHW4c", "NHWC")  # To NHWC
        return relay.Function([x], y)

    def before5():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.layout_transform(x, "NHWC", "NCHW")  # To NCHW
        y = relay.layout_transform(y, "NCHW", "NCHW4c")  # To NCHW4c
        y = relay.nn.relu(y)
        y = relay.layout_transform(y, "NCHW4c", "NCHW")  # To NCHW
        y = relay.layout_transform(y, "NCHW", "NHWC")  # To NHWC
        return relay.Function([x], y)

    def expected5():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.layout_transform(x, "NHWC", "NCHW4c")  # To NCHW4c
        y = relay.nn.relu(y)
        y = relay.layout_transform(y, "NCHW4c", "NHWC")  # To NHWC
        return relay.Function([x], y)

    def before6():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.layout_transform(x, "NCHW", "NHWC")
        y = relay.layout_transform(y, "NHWC", "NCHW")
        y = relay.nn.relu(y)
        return relay.Function([x], y)

    def expected6():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.nn.relu(x)
        return relay.Function([x], y)

    def before7():
        x = relay.var("x", shape=(1, 32, 56, 56, 4), dtype="float32")  # NCHW4c
        y = relay.layout_transform(x, "NCHW4c", "NCHW8c")
        y = relay.layout_transform(y, "NCHW8c", "NCHW4c")
        y = relay.nn.relu(y)
        return relay.Function([x], y)

    def expected7():
        x = relay.var("x", shape=(1, 32, 56, 56, 4), dtype="float32")  # NCHW4c
        y = relay.nn.relu(x)
        return relay.Function([x], y)

    def before8():
        x = relay.var("x", shape=(1, 32, 56, 56, 4), dtype="float32")  # NCHW4c
        y = relay.layout_transform(x, "NCHW4c", "NCHW")
        y = relay.layout_transform(y, "NCHW", "NCHW8c")
        y = relay.nn.relu(y)
        return relay.Function([x], y)

    def expected8():
        x = relay.var("x", shape=(1, 32, 56, 56, 4), dtype="float32")  # NCHW4c
        y = relay.layout_transform(x, "NCHW4c", "NCHW8c")
        y = relay.nn.relu(y)
        return relay.Function([x], y)

    def before9():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.layout_transform(x, "NCHW", "NCHW4c")  # To NCHW4c
        y = relay.layout_transform(y, "NCHW4c", "NCHW")  # To NCHW
        y = relay.nn.relu(y)
        return relay.Function([x], y)

    def expected9():
        x = relay.var("x", shape=(1, 56, 56, 128), dtype="float32")  # NHWC
        y = relay.nn.relu(x)
        return relay.Function([x], y)

    def before10():
        x = relay.var("x", shape=(1, 128, 56, 56), dtype="float32")  # NHWC
        y = relay.layout_transform(x, "NCHW", "NHWC")
        y = relay.layout_transform(y, "NHWC", "CHWN")
        y = relay.nn.relu(y)
        return relay.Function([x], y)

    def expected10():
        x = relay.var("x", shape=(1, 128, 56, 56), dtype="float32")  # NCHW
        y = relay.transpose(x, axes=[1, 2, 3, 0])  # To CHWN
        y = relay.nn.relu(y)
        return relay.Function([x], y)

    for before, expected in [
        [before1(), expected1()],
        [before2(), expected2()],
        [before3(), expected3()],
        [before4(), expected4()],
        [before5(), expected5()],
        [before6(), expected6()],
        [before7(), expected7()],
        [before8(), expected8()],
        [before9(), expected9()],
        [before10(), expected10()],
    ]:
        after = run_opt_pass(before, transform.SimplifyExpr())
        expected = run_opt_pass(expected, transform.InferType())
        assert tvm.ir.structural_equal(after, expected), "\nafter: {} \nexpected: {}".format(
            after, expected
        )


def test_simplify_full_elementwise():
    def validate(shape, value, dtype):
        def before_left(x, elem_op, full):
            return elem_op(full, x)

        def after_left(x, elem_op, value):
            return elem_op(relay.const(value, dtype), x)

        def before_right(x, elem_op, full):
            return elem_op(x, full)

        def after_right(x, elem_op, value):
            return elem_op(x, relay.const(value, dtype))

        x = relay.var("x", shape=shape, dtype=dtype)
        elem_ops = [relay.add, relay.multiply, relay.subtract, relay.divide]
        full_ops = []
        if value == 0:
            full_ops.append(relay.zeros(shape, dtype))
            full_ops.append(relay.zeros_like(x))
        if value == 1:
            full_ops.append(relay.ones(shape, dtype))
            full_ops.append(relay.ones_like(x))
        else:
            full_ops.append(relay.full(relay.const(value, dtype), shape))
            full_ops.append(relay.full_like(x, relay.const(value, dtype)))
        for op in elem_ops:
            for full in full_ops:
                z = before_left(x, op, full)
                zz = run_opt_pass(z, transform.SimplifyExpr())
                after = run_opt_pass(after_left(x, op, value), transform.InferType())
                assert tvm.ir.structural_equal(zz, after)

                z = before_right(x, op, full)
                zz = run_opt_pass(z, transform.SimplifyExpr())
                after = run_opt_pass(after_right(x, op, value), transform.InferType())
                assert tvm.ir.structural_equal(zz, after)

        # Test the case in which x is broadcast to full's shape
        full_ops = []
        if value == 0:
            full_ops.append(relay.zeros(shape * 2, dtype))
        if value == 1:
            full_ops.append(relay.ones(shape * 2, dtype))
        else:
            full_ops.append(relay.full(relay.const(value, dtype), shape * 2))
        for op in elem_ops:
            for full in full_ops:
                z = before_left(x, op, full)
                zz = run_opt_pass(z, transform.SimplifyExpr())
                after = run_opt_pass(before_left(x, op, full), transform.InferType())
                assert tvm.ir.structural_equal(zz, after)

                z = before_right(x, op, full)
                zz = run_opt_pass(z, transform.SimplifyExpr())
                after = run_opt_pass(before_right(x, op, full), transform.InferType())
                assert tvm.ir.structural_equal(zz, after)

    for shape in [[10], [10, 10], [10, 10, 10]]:
        for dtype in ["float32", "int32", "bool"]:
            for value in [0, 1, 2]:
                validate(shape, value, dtype)


if __name__ == "__main__":
    test_simplify_reshape()
    test_simplify_transpose()
    test_simplify_full_elementwise()
