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

import os
import tvm
import numpy as np
from tvm import relay
from tvm.relay import testing
from tvm.relay.transform import recast
from tvm.relay.transform import recast
from tvm.contrib import graph_runtime
from tvm import autotvm

def get_reference(mod, params1, input_shape, inputs):
    mod_fp32 = recast(mod, "float32", "float32", ops = ["nn.conv2d", "add", "multiply", "nn.relu"])
    with relay.build_config(opt_level=3):
        graph, lib, params = relay.build(
            mod_fp32, "llvm", params=params1
        )
    ctx = tvm.cpu()
    m = graph_runtime.create(graph, lib, ctx)
    if isinstance(input_shape, dict):
        for key in input_shape:
            m.set_input(key, inputs[-1])
    else:
        m.set_input("data", inputs[-1])
    m.set_input(**params)
    m.run()
    return [m.get_output(0).asnumpy(),]


# build module run with opencl and cpu, compare results
def build_run_compare(
    tvm_mod,
    params1,
    input_shape,
    dtype="float32",
    target="llvm",
    json=None):

    rpc_tracker_host = os.environ["TVM_TRACKER_HOST"]
    rpc_tracker_port = os.environ["TVM_TRACKER_PORT"]
    if rpc_tracker_host:
        run_on_host = 0
        target_host = "llvm -mtriple=arm64-linux-android"
        rpc_tracker_port = int(rpc_tracker_port)
    else:
        run_on_host = 1
        target_host="llvm"

    #
    if json:
        with autotvm.apply_history_best(json):
            with relay.build_config(opt_level=3):
                graph, lib, params = relay.build(
                    tvm_mod, target_host=target_host, target=target, params=params1
                )
    else:
            with relay.build_config(opt_level=3):
                graph, lib, params = relay.build(
                    tvm_mod, target_host=target_host, target=target, params=params1
                )

    if run_on_host:
        ctx = tvm.opencl()
        m = graph_runtime.create(graph, lib, ctx)
    else:
        from tvm import rpc
        from tvm.contrib import utils, ndk
        rpc_key = "android"
        tracker = rpc.connect_tracker(rpc_tracker_host, rpc_tracker_port)
        remote = tracker.request(
            rpc_key, priority=0, session_timeout=600
        )
        temp = utils.tempdir()
        dso_binary = "dev_lib_cl.so"
        dso_binary_path = temp.relpath(dso_binary)
        ctx = remote.cl(0)
        lib.export_library(dso_binary_path, ndk.create_shared)
        remote.upload(dso_binary_path)
        rlib = remote.load_module(dso_binary)
        m = graph_runtime.create(graph, rlib, ctx)
    m.set_input(**params)
    inputs = []
    if isinstance(input_shape, dict):
        for key in input_shape:
            inputs.append(np.random.normal(size=input_shape[key]).astype(dtype))
            m.set_input(key, inputs[-1])
    else:
        inputs.append(np.random.normal(size=input_shape).astype(dtype))
        m.set_input("data", inputs[-1])
    m.run()

    ref_outputs = get_reference(tvm_mod, params1, input_shape, inputs)
    for i, ref_output in enumerate(ref_outputs):
        tvm_output = m.get_output(i)
        output = tvm_output.asnumpy()
        # for index, x in np.ndenumerate(ref_output):
        #     if abs(output[index] - x) > 0.01:
        #         print(index, output[index], x)

        np.testing.assert_allclose(output, ref_output, rtol=1e-2, atol=1e-2)


def test_conv2d_resnet50_v2_nchw_3c():
    target="opencl --device=adreno"
    dtype="float16"

    input_shape = (1, 3, 224, 224)
    filter_shape = (64, 3, 7, 7)
    bias_shape = (1, 64, 1, 1)
    A = relay.var("data", shape=input_shape, dtype=dtype)
    B = relay.var("weight", shape=filter_shape, dtype=dtype)
    bias = relay.var("bias", shape=bias_shape, dtype=dtype)

    #C = relay.nn.relu(A)
    conv = relay.nn.conv2d(A, B, data_layout="NCHW", kernel_layout="OIHW",
                        padding=[3,3,3,3],strides=[2,2],
                        out_dtype=dtype, channels=64, kernel_size=(7,7))
    D = relay.op.add(conv, bias)
    D = relay.op.nn.relu(D)

    mod = relay.Function([A, B, bias], D)
    # mod, params = relay.testing.init.create_workload(func)
    np.random.seed(1)
    initializer = relay.testing.init.Xavier()
    filter_data = np.zeros(filter_shape).astype(dtype)
    bias_data = np.zeros(bias_shape).astype(dtype)
    initializer("weight", filter_data)
    initializer("bias", bias_data)
    params1 = {
        "weight": tvm.nd.array(filter_data),
        "bias" : tvm.nd.array(bias_data),
    }

    build_run_compare (mod, params1, {"data": input_shape}, dtype, target)

def test_conv2d_inceptionv3_nchw_3c():
    target="opencl --device=adreno"
    dtype="float16"

    input_shape = (1, 3, 299, 299)
    filter_shape = (64, 3, 3, 3)
    bias_shape = (1, 64, 1, 1)
    A = relay.var("data", shape=input_shape, dtype=dtype)
    B = relay.var("weight", shape=filter_shape, dtype=dtype)
    bias = relay.var("bias", shape=bias_shape, dtype=dtype)

    #C = relay.nn.relu(A)
    conv = relay.nn.conv2d(A, B, data_layout="NCHW", kernel_layout="OIHW",
                        padding=[0,0,0,0],strides=[2,2],
                        out_dtype=dtype, channels=64, kernel_size=(3,3))
    D = relay.op.add(conv, bias)
    D = relay.op.nn.relu(D)

    mod = relay.Function([A, B, bias], D)  
    np.random.seed(0)
    initializer = relay.testing.init.Xavier()
    filter_data = np.zeros(filter_shape).astype(dtype)
    bias_data = np.zeros(bias_shape).astype(dtype)
    initializer("weight", filter_data)
    initializer("bias", bias_data)
    params1 = {
        "weight": tvm.nd.array(filter_data),
        "bias" : tvm.nd.array(bias_data),
    }

    build_run_compare (mod, params1, {"data": input_shape}, dtype, target)

def test_conv2d_1x1_16c16spatial():
    target="opencl --device=adreno"
    dtype="float16"

    input_shape = (1, 16, 256, 256)
    filter_shape = (32, 16, 4, 4)
    bias_shape = (1, 32, 1, 1)
    A = relay.var("data", shape=input_shape, dtype=dtype)
    B = relay.var("weight", shape=filter_shape, dtype=dtype)
    bias = relay.var("bias", shape=bias_shape, dtype=dtype)

    #C = relay.nn.relu(A)
    conv = relay.nn.conv2d(A, B, data_layout="NCHW", kernel_layout="OIHW",
                        padding=[0,0,0,0],strides=[2,2],
                        out_dtype=dtype, channels=32, kernel_size=(4,4))
    D = relay.op.add(conv, bias)
    D = relay.op.nn.relu(D)

    mod = relay.Function([A, B, bias], D)  
    np.random.seed(0)
    initializer = relay.testing.init.Xavier()
    filter_data = np.zeros(filter_shape).astype(dtype)
    bias_data = np.zeros(bias_shape).astype(dtype)
    initializer("weight", filter_data)
    initializer("bias", bias_data)
    params1 = {
        "weight": tvm.nd.array(filter_data),
        "bias" : tvm.nd.array(bias_data),
    }

    build_run_compare (mod, params1, {"data": input_shape}, dtype, target)

def test_conv2d_4x4_16c16pad():
    target="opencl --device=adreno"
    dtype="float16"

    input_shape = (1, 32, 256, 256)
    filter_shape = (32, 32, 4, 4)
    bias_shape = (1, 32, 1, 1)
    A = relay.var("data", shape=input_shape, dtype=dtype)
    B = relay.var("weight", shape=filter_shape, dtype=dtype)
    bias = relay.var("bias", shape=bias_shape, dtype=dtype)

    #C = relay.nn.relu(A)
    conv = relay.nn.conv2d(A, B, data_layout="NCHW", kernel_layout="OIHW",
                        padding=[3,3,0,0],strides=[2,2],
                        out_dtype=dtype, channels=32, kernel_size=(4,4))
    D = relay.op.add(conv, bias)
    D = relay.op.nn.relu(D)

    mod = relay.Function([A, B, bias], D)  
    np.random.seed(0)
    initializer = relay.testing.init.Xavier()
    filter_data = np.zeros(filter_shape).astype(dtype)
    bias_data = np.zeros(bias_shape).astype(dtype)
    initializer("weight", filter_data)
    initializer("bias", bias_data)
    params1 = {
        "weight": tvm.nd.array(filter_data),
        "bias" : tvm.nd.array(bias_data),
    }

    build_run_compare (mod, params1, {"data": input_shape}, dtype, target)


def test_conv2d_yolov3_v2_nchw_3c():
    target="opencl --device=adreno"
    dtype="float16"

    input_shape = (1, 1024, 13, 13)
    filter_shape = (255, 1024, 1, 1)
    A = relay.var("data", shape=input_shape, dtype=dtype)
    B = relay.var("weight", shape=filter_shape, dtype=dtype)

    conv = relay.nn.conv2d(A, B, data_layout="NCHW", kernel_layout="OIHW",
                        padding=[0,0,0,0],strides=[1,1],
                        out_dtype=dtype, channels=255, kernel_size=(1,1))

    mod = relay.Function([A, B], conv)
    # mod, params = relay.testing.init.create_workload(func)
    np.random.seed(0)
    initializer = relay.testing.init.Xavier()
    filter_data = np.zeros(filter_shape).astype(dtype)
    initializer("weight", filter_data)
    params = {
        "weight": tvm.nd.array(filter_data),
    }

    build_run_compare (mod, params, {"data": input_shape}, dtype, target)

if __name__ == "__main__":
  test_conv2d_resnet50_v2_nchw_3c()
  test_conv2d_inceptionv3_nchw_3c()
  test_conv2d_yolov3_v2_nchw_3c()
  test_conv2d_1x1_16c16spatial()
  test_conv2d_4x4_16c16pad()