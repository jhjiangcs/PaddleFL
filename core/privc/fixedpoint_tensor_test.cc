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

#include <string>
#include <cmath>

#include "gtest/gtest.h"
#include "paddle/fluid/framework/tensor.h"
#include "paddle/fluid/framework/operator.h"
#include "paddle/fluid/framework/scope.h"

#include "./privc_context.h"
#include "core/paddlefl_mpc/mpc_protocol/network/mesh_network.h"
#include "core/paddlefl_mpc/mpc_protocol/context_holder.h"
#include "fixedpoint_tensor.h"
#include "core/common/paddle_tensor.h"

namespace privc {

using g_ctx_holder = paddle::mpc::ContextHolder;
using Fix64N32 = FixedPointTensor<int64_t, PRIVC_FIXED_POINT_SCALING_FACTOR>;
using AbstractContext = paddle::mpc::AbstractContext;

class FixedTensorTest : public ::testing::Test {
public:
    static paddle::platform::CPUDeviceContext _cpu_ctx;
    static std::shared_ptr<paddle::framework::ExecutionContext> _exec_ctx;
    static std::shared_ptr<AbstractContext> _mpc_ctx[2];
    static std::shared_ptr<gloo::rendezvous::HashStore> _store;
    static std::thread _t[2];
    static std::shared_ptr<TensorAdapterFactory> _s_tensor_factory;

    virtual ~FixedTensorTest() noexcept {}

    static void SetUpTestCase() {

        paddle::framework::OperatorBase* op = nullptr;
        paddle::framework::Scope scope;
        paddle::framework::RuntimeContext ctx({}, {});

        _exec_ctx = std::make_shared<paddle::framework::ExecutionContext>(
            *op, scope, _cpu_ctx, ctx);
        _store = std::make_shared<gloo::rendezvous::HashStore>();

        for (size_t i = 0; i < 2; ++i) {
            _t[i] = std::thread(&FixedTensorTest::gen_mpc_ctx, i);
        }
        for (auto& ti : _t) {
            ti.join();
        }

        _s_tensor_factory = std::make_shared<common::PaddleTensorFactory>(&_cpu_ctx);
    }

    static inline std::shared_ptr<paddle::mpc::MeshNetwork> gen_network(size_t idx) {
        return std::make_shared<paddle::mpc::MeshNetwork>(idx,
                                                          "127.0.0.1",
                                                          2,
                                                          "test_prefix_privc",
                                                          _store);
    }
    static inline void gen_mpc_ctx(size_t idx) {
        auto net = gen_network(idx);
        net->init();
        _mpc_ctx[idx] = std::make_shared<PrivCContext>(idx, net);
    }

    std::shared_ptr<TensorAdapter<int64_t>> gen(std::vector<size_t> shape) {
        return _s_tensor_factory->template create<int64_t>(shape);
    }
};

std::shared_ptr<TensorAdapter<int64_t>> gen(std::vector<size_t> shape) {
    return g_ctx_holder::tensor_factory()->template create<int64_t>(shape);
}

TEST_F(FixedTensorTest, share) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl = gen(shape);
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };

    TensorAdapter<int64_t>* output_share[2] = {ret[0].get(), ret[1].get()};
    sl->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl->scaling_factor() = PRIVC_FIXED_POINT_SCALING_FACTOR;

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                Fix64N32::share(sl.get(), output_share);
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){});
        }
    );
    for (auto &t: _t) {
        t.join();
    }

    auto p = gen(shape);
    output_share[0]->add(output_share[1], p.get());
    EXPECT_EQ(1, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, reveal) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    // lhs = 3 = 1 + 2
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p0 = gen(shape);
    auto p1 = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.reveal(p0.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.reveal(p1.get());
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(p0->data()[0], p1->data()[0]);
    EXPECT_EQ(3, p0->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, addplain) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr = gen(shape);
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 3 = 1 + 2
    // rhs = 3
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr->data()[0] = (int64_t)3 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    sr->scaling_factor() = PRIVC_FIXED_POINT_SCALING_FACTOR;
    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.add(sr.get(), &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.add(sr.get(), &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(6, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, addfixed) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 3 = 1 + 2
    // rhs = 3 = 1 + 2
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fr0(sr[0].get());
    Fix64N32 fr1(sr[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.add(&fr0, &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.add(&fr1, &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(6, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, subplain) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr = gen(shape);
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 3 = 1 + 2
    // rhs = 2
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    sr->scaling_factor() = PRIVC_FIXED_POINT_SCALING_FACTOR;
    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.sub(sr.get(), &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.sub(sr.get(), &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(1, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, subfixed) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 3 = 1 + 2
    // rhs = 2 = 1 + 1
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[1]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fr0(sr[0].get());
    Fix64N32 fr1(sr[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.sub(&fr0, &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.sub(&fr1, &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(1, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, negative) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 3 = 1 + 2
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.negative(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.negative(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(-3, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, exp) {
    std::vector<size_t> shape = { 2 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = (3,-2)
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.exp(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.exp(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_NEAR(std::exp(3.0), p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 1);
    EXPECT_NEAR(std::exp(-2.0), p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.1);
}

TEST_F(FixedTensorTest, mulfixed) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 2, -2.23
    // rhs = 2, 2

    sl[0]->data()[0] = (int64_t)(-1.23 * pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    sl[1]->data()[0] = -(int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    sr[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[1]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fr0(sr[0].get());
    Fix64N32 fr1(sr[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.mul(&fr0, &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.mul(&fr1, &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }

    EXPECT_NEAR(-4.46, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
}

TEST_F(FixedTensorTest, mulfixed_upper_bound) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 2^16
    // rhs = 2^16
    sl[0]->data()[0] = (int64_t)1 << (PRIVC_FIXED_POINT_SCALING_FACTOR + 15);
    sl[1]->data()[0] = (int64_t)1 << (PRIVC_FIXED_POINT_SCALING_FACTOR + 15);
    sr[0]->data()[0] = (int64_t)1 << (PRIVC_FIXED_POINT_SCALING_FACTOR + 15);
    sr[1]->data()[0] = (int64_t)1 << (PRIVC_FIXED_POINT_SCALING_FACTOR + 15);

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fr0(sr[0].get());
    Fix64N32 fr1(sr[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.mul(&fr0, &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.mul(&fr1, &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_NEAR(0, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
}

TEST_F(FixedTensorTest, mulplain) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr = gen(shape);
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 2 = 1 + 1
    // rhs = 2 = 1 + 1
    sl[0]->data()[0] = (int64_t)(-1.23 * pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    sl[1]->data()[0] = -(int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;


    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());

    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.mul(sr.get(), &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.mul(sr.get(), &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_NEAR(-4.46, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
}

TEST_F(FixedTensorTest, sum) {
    std::vector<size_t> shape = { 2 };
    std::vector<size_t> shape_ret = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape_ret), gen(shape_ret) };
    // lhs = (3, 3)
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape_ret);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.sum(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.sum(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(6, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, divplain) {
    std::vector<size_t> shape = { 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr = gen(shape);
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = 2 = 1 + 1
    // rhs = 4
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr->data()[0] = (int64_t)4 << PRIVC_FIXED_POINT_SCALING_FACTOR;


    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());

    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.div(sr.get(), &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.div(sr.get(), &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_NEAR(0.5, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
}

TEST_F(FixedTensorTest, mat_mulfixed) {
    std::vector<size_t> shape = { 2, 2 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> sr[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = [2, 3, 4, 5]
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[2] = (int64_t)3 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[3] = (int64_t)4 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[2] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[3] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    // rhs = [0, -1, -2, -3]
    sr[0]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[0]->data()[1] = (int64_t)-2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[0]->data()[2] = (int64_t)-3 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[0]->data()[3] = (int64_t)-4 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[1]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[1]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[1]->data()[2] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sr[1]->data()[3] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fr0(sr[0].get());
    Fix64N32 fr1(sr[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.mat_mul(&fr0, &fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.mat_mul(&fr1, &fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_NEAR(-6, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
    EXPECT_NEAR(-11, p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
    EXPECT_NEAR(-10, p->data()[2] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
    EXPECT_NEAR(-19, p->data()[3] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.00001);
}

TEST_F(FixedTensorTest, relu) {
    std::vector<size_t> shape = { 2 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = [-2, 2]
    sl[0]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.relu(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.relu(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(0, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(2, p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}


TEST_F(FixedTensorTest, sigmoid) {
    std::vector<size_t> shape = { 3 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = [-1, 0, 1]
    sl[0]->data()[0] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[2] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)-2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[2] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.sigmoid(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.sigmoid(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(0, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(0.5, p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(1, p->data()[2] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, argmax) {
    std::vector<size_t> shape = { 2, 2 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = [[-2, 2], [1, 0]]
    sl[0]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[2] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[3] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[2] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[3] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.argmax(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.argmax(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(0, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(1, p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(1, p->data()[2] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(0, p->data()[3] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, argmax_size_one) {
    std::vector<size_t> shape = { 2, 1 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = [[-2], [2]]
    sl[0]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.argmax(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.argmax(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(1, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(1, p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));

}

TEST_F(FixedTensorTest, reduce) {
    std::vector<size_t> shape = { 2, 2 };
    std::vector<size_t> ret_shape = { 2 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(ret_shape), gen(ret_shape) };
    // lhs = [[-2, 2], [1, 0]]
    sl[0]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[2] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[3] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[2] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[3] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    auto p = gen(ret_shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.reduce(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.reduce(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_EQ(0, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
    EXPECT_EQ(1, p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR));
}

TEST_F(FixedTensorTest, softmax) {
    std::vector<size_t> shape = { 2, 2 };
    std::shared_ptr<TensorAdapter<int64_t>> sl[2] = { gen(shape), gen(shape) };
    std::shared_ptr<TensorAdapter<int64_t>> ret[2] = { gen(shape), gen(shape) };
    // lhs = [[-2, 2], [1, 0]]
    sl[0]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[2] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[0]->data()[3] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[0] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[1] = (int64_t)1 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[2] = (int64_t)2 << PRIVC_FIXED_POINT_SCALING_FACTOR;
    sl[1]->data()[3] = (int64_t)-1 << PRIVC_FIXED_POINT_SCALING_FACTOR;

    auto p = gen(shape);

    Fix64N32 fl0(sl[0].get());
    Fix64N32 fl1(sl[1].get());
    Fix64N32 fout0(ret[0].get());
    Fix64N32 fout1(ret[1].get());

    _t[0] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[0], [&](){
                fl0.softmax(&fout0);
                fout0.reveal_to_one(0, p.get());
            });
        }
    );
    _t[1] = std::thread(
        [&] () {
        g_ctx_holder::template run_with_context(
            _exec_ctx.get(), _mpc_ctx[1], [&](){
                fl1.softmax(&fout1);
                fout1.reveal_to_one(0, nullptr);
            });
        }
    );
    for (auto &t: _t) {
        t.join();
    }
    EXPECT_NEAR(0.01798, p->data()[0] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.01);
    EXPECT_NEAR(0.9820, p->data()[1] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.01);
    EXPECT_NEAR(0.7311, p->data()[2] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.01);
    EXPECT_NEAR(0.2689, p->data()[3] / std::pow(2, PRIVC_FIXED_POINT_SCALING_FACTOR), 0.01);
}

paddle::platform::CPUDeviceContext privc::FixedTensorTest::_cpu_ctx;
std::shared_ptr<paddle::framework::ExecutionContext> privc::FixedTensorTest::_exec_ctx;
std::shared_ptr<AbstractContext> privc::FixedTensorTest::_mpc_ctx[2];
std::shared_ptr<gloo::rendezvous::HashStore> privc::FixedTensorTest::_store;
std::thread privc::FixedTensorTest::_t[2];
std::shared_ptr<TensorAdapterFactory> privc::FixedTensorTest::_s_tensor_factory;

} // namespace privc
