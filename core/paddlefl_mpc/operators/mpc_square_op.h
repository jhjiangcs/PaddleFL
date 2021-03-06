/* Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once
#include "mpc_op.h"

namespace paddle {
namespace operators {

using Tensor = framework::Tensor;

template <typename DeviceContext, typename T>
class MpcSquareKernel : public MpcOpKernel<T> {
public:
    void ComputeImpl(const framework::ExecutionContext &ctx) const override {
        auto *in_x_t = ctx.Input<Tensor>("X");
        auto *out_t = ctx.Output<Tensor>("Out");
        out_t->mutable_data<T>(ctx.GetPlace());
        mpc::MpcInstance::mpc_instance()->mpc_protocol()->mpc_operators()->elementwise_mul(in_x_t, in_x_t, out_t);
    }
};

template <typename DeviceContext, typename T>
class MpcSquareGradKernel : public MpcOpKernel<T> {
public:
    void ComputeImpl(const framework::ExecutionContext &ctx) const override {
        auto *in_x_t = ctx.Input<Tensor>("X");
        auto *dout_t = ctx.Input<Tensor>(framework::GradVarName("Out"));
        auto *dx_t = ctx.Output<Tensor>(framework::GradVarName("X"));
        if (dx_t != nullptr) {
            // allocate memory on device.
            dx_t->mutable_data<T>(ctx.GetPlace());
            // dx = dout * 2 * x
            mpc::MpcInstance::mpc_instance()->mpc_protocol()->mpc_operators()->scale(in_x_t, 2.0, dx_t);
            mpc::MpcInstance::mpc_instance()->mpc_protocol()->mpc_operators()->elementwise_mul(dx_t, dout_t, dx_t);
        }
    }
};

}  // namespace operators
}  // namespace paddle

