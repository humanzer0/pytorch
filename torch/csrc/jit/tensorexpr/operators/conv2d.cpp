#include <torch/csrc/jit/tensorexpr/loopnest.h>
#include <torch/csrc/jit/tensorexpr/operators/conv2d.h>
#include <torch/csrc/jit/tensorexpr/tensor.h>

namespace torch {
namespace jit {
namespace tensorexpr {

void assert_dims_constant(const BufHandle& buf) {
  for (auto const& dim : buf.node()->dims()) {
    TORCH_INTERNAL_ASSERT(dim->isConstant());
  }
}

Tensor* conv2d_depthwise(
    BufHandle input,
    BufHandle weight,
    BufHandle bias,
    int stride,
    int pad,
    int groups) {
  TORCH_INTERNAL_ASSERT(input.ndim() == 4);
  TORCH_INTERNAL_ASSERT(weight.ndim() == 4);

  assert_dims_constant(input);
  assert_dims_constant(weight);
  assert_dims_constant(bias);

  auto const& N = immediateAs<int>(input.dim(0));
  auto const& C = immediateAs<int>(input.dim(1));
  auto const& H = immediateAs<int>(input.dim(2));
  auto const& W = immediateAs<int>(input.dim(3));

  auto const& K = immediateAs<int>(weight.dim(0));
  auto const& CperG = immediateAs<int>(weight.dim(1));
  auto const& R = immediateAs<int>(weight.dim(2));
  auto const& S = immediateAs<int>(weight.dim(3));

  TORCH_INTERNAL_ASSERT(C == K && K == groups && CperG == 1);
  TORCH_INTERNAL_ASSERT(R == S);

  auto OH = (H - R + 2 * pad) / stride + 1;
  auto OW = (W - S + 2 * pad) / stride + 1;

  Tensor* conv = Reduce(
      "conv2d_depthwise",
      {{N, "n"}, {K, "k"}, {OH, "oh"}, {OW, "ow"}},
      Sum(),
      [&](const std::vector<VarHandle>& v) {
        auto const& k = v[1];
        return bias.load(k);
      },
      [&](const std::vector<VarHandle>& v) {
        auto const& n = v[0];
        auto const& k = v[1];
        auto const& oh = v[2];
        auto const& ow = v[3];
        auto const& c = v[4];
        auto const& r = v[5];
        auto const& s = v[6];
        auto cond = CompareSelect::make(oh * stride - pad + r, 0, 1, 0, kLT);
        cond = CompareSelect::make(ow * stride - pad + s, 0, 1, cond, kLT);
        cond = CompareSelect::make(oh * stride - pad + r, H, 1, cond, kGE);
        cond = CompareSelect::make(ow * stride - pad + s, W, 1, cond, kGE);
        auto in = ifThenElse(
            cond,
            0.f,
            input.load(n, k, oh * stride - pad + r, ow * stride - pad + s));
        return in * weight.load(k, c, r, s);
      },
      {{C / groups, "c"}, {R, "r"}, {S, "s"}});

  LoopNest nest({conv});

  constexpr int kLoopH = 2, kLoopW = 3;
  if (R == 3 && stride == 2 && pad == 1) {
    For *head, *tail;
    auto loops = nest.getLoopStmtsFor(conv);
    nest.sliceHead(loops[kLoopW], 2, &head, &tail);
    loops = nest.getLoopStmtsFor(conv);
    nest.sliceHead(loops[kLoopH], 2, &head, &tail);
  } else if (R == 3 && stride == 1 && pad == 1) {
    For *main, *peeled;
    auto loops = nest.getAllLoopNestsWritingToBuf(conv->buf());
    main = loops[1][kLoopW];
    nest.sliceHead(main, 1, &peeled, &main);
    nest.sliceTail(main, 1, &main, &peeled);
    main = LoopNest::getParentLoop(main);
    nest.sliceHead(main, 1, &peeled, &main);
    nest.sliceTail(main, 1, &main, &peeled);
  }

  return new Tensor(conv->buf(), nest.root_stmt());
}

} // namespace tensorexpr
} // namespace jit
} // namespace torch
