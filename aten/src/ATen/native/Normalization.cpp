#include <ATen/ATen.h>
#include <ATen/MetaFunctions.h>
#include <ATen/NativeFunctions.h>
#include <ATen/AccumulateType.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/Parallel.h>
#include <ATen/Config.h>

#include <ATen/detail/CUDAHooksInterface.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>
#include <ATen/native/batch_norm.h>
#include <ATen/native/Normalization.h>

#include <vector>

static const int MIOPEN_DIM_MAX = 5;

namespace at {
namespace meta {

TORCH_META_FUNC(renorm)(const Tensor& self, const Scalar& p, int64_t dim, const Scalar& maxnorm) {
  TORCH_CHECK(!p.isComplex(), "renorm: p must be real-valued");
  TORCH_CHECK(p.toDouble() > 0.0, "renorm: non-positive-norm not supported");
  TORCH_CHECK(!maxnorm.isComplex(), "renorm: maxnorm must be real-valued");
  TORCH_CHECK(maxnorm.toDouble() >= 0.0,
              "renorm: expected maxnorm to be >= 0 but got ", maxnorm.toDouble());
  const auto ndim = self.dim();
  TORCH_CHECK(ndim > 1, "renorm: input needs at least 2 dimensions, got ", ndim, "dimensions");
  set_output(self.sizes(), self.options());
}

}  // namespace meta

namespace native {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_DISPATCH(batch_norm_cpu_inference_contiguous_stub);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_DISPATCH(renorm_scale_factor_stub);

namespace {
  void check_dims_match_num_input_features(const char* arg_name, int64_t expected, int64_t actual){
    TORCH_CHECK(actual == expected,
             arg_name, " should contain ", expected, " elements not ", actual);
  }

  static inline Tensor repeat_if_defined(const Tensor& t, int64_t repeat) {
    if (t.defined()) {
      return t.repeat(repeat);
    }
    return t;
  }
}

// TensorAccessor when it is defined to work around undefined...
template <typename scalar_t>
static TensorAccessor<scalar_t, 1> conditional_accessor_1d(const Tensor& t) {
  if (! t.defined()) {
    return TensorAccessor<scalar_t, 1>(nullptr, nullptr, nullptr);
  }
  return t.accessor<scalar_t, 1>();
}

template<typename T>
struct InvStd {
  T operator()(T var, double epsilon) const {
    T invstd = 0;
    if (var != static_cast<T>(0) || epsilon != static_cast<T>(0)) {
      invstd = static_cast<T>(1) / std::sqrt(var + epsilon);
    }
    return invstd;
  }
};

template<typename T>
struct Var {
  T operator()(T var, double epsilon) const {
    return var;
  }
};

template<typename scalar_t>
void batch_norm_cpu_inference_collect_linear_and_constant_terms(
    scalar_t* alpha, scalar_t* beta, int64_t n_channel,
    const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    const Tensor& mean, const Tensor& variance, double eps) {

  const scalar_t* weight_data = weight.defined() ? weight.data_ptr<scalar_t>() : nullptr;
  const scalar_t* bias_data = bias.defined() ? bias.data_ptr<scalar_t>() : nullptr;
  const scalar_t* mean_data = mean.data_ptr<scalar_t>();
  const scalar_t* var_data = variance.data_ptr<scalar_t>();

  /// Collect the linear and constant terms regarding the input.
  /// output(n, c, h, w)
  ///     = (input(n, c, h, w) - mean(c)) / sqrt(var(c) + eps) * weight(c)
  ///         + bias(c)
  ///     = input(n, c, h, w) * inv_var(c) * weight(c)
  ///         - mean(c) * inv_var(c) * weight(c) + bias(c),
  /// where inv_var(c) = 1 / sqrt(var(c) + eps).
  /// So the linear term, alpha(c) = inv_var(c) * weight(c),
  ///   the constant term beta(c) = bias(c) - mean(c) * inv_var(c) * weight(c)
  /// Note that this is only a good idea if (input_size >> c), in degenerate
  /// cases where image_size == 1 && batch_size == 1, it is slow.
  for (int64_t c = 0; c < n_channel; c++) {
    scalar_t inv_var = 1 / std::sqrt(var_data[c] + static_cast<scalar_t>(eps));
    scalar_t weight_v = weight_data ? weight_data[c] : 1;
    scalar_t bias_v = bias_data ? bias_data[c] : 0;
    alpha[c] = inv_var * weight_v;
    beta[c] = bias_v - mean_data[c] * inv_var * weight_v;
  }
}

/// A fast path for CPU inference when all tensors are channels last contiguous.
/// This code achieves machine bandwidth peak without AVX support.
/// If this changes for future architectures, we can move it to the cpu/
/// directory.
template<typename scalar_t>
void batch_norm_cpu_inference_channels_last(Tensor& output, const Tensor& input,
    const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    const Tensor& mean, const Tensor& variance, double eps) {

  int64_t n_batch = input.size(0);
  int64_t n_channel = input.size(1);
  int64_t image_size = input.numel() / n_batch / n_channel;

  scalar_t* output_data = output.data_ptr<scalar_t>();
  const scalar_t* input_data = input.data_ptr<scalar_t>();

  Tensor alpha = at::empty_like(mean, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  Tensor beta = at::empty_like(mean, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  scalar_t* alpha_data = alpha.data_ptr<scalar_t>();
  scalar_t* beta_data = beta.data_ptr<scalar_t>();

  batch_norm_cpu_inference_collect_linear_and_constant_terms<scalar_t>(
      alpha_data, beta_data, n_channel, weight, bias, mean, variance, eps);

  // Apply the linear terms to the input,
  // output(n, c, h, w) = input(n, c, h, w) * alpha(c) + beta(c)
  // No need to use parallel_for as this function is supposed to be
  // memory-limited.
  // Keep the loop structure simple to make sure compiler vectorization kicks in.
  if (n_channel != 1) {
    for (int64_t n = 0; n < n_batch; ++n) {
      for (int64_t i = 0; i < image_size; ++i) {
        for (int64_t c = 0; c < n_channel; ++c) {
          // Keep all the offset calculation within the inner loop for
          // simplicity. Compilers are very good at hoisting the common part
          // outside.
          int64_t offset = n * image_size * n_channel + i * n_channel + c;
          output_data[offset] = input_data[offset] * alpha_data[c] + beta_data[c];
        }
      }
    }
  } else {
    // n_channel == 1
    for (int64_t n = 0; n < n_batch; ++n) {
      for (int64_t i = 0; i < image_size; ++i) {
        int64_t offset = n * image_size + i;
        output_data[offset] = input_data[offset] * alpha_data[0] + beta_data[0];
      }
    }
  }
}

template<typename scalar_t>
std::tuple<Tensor,Tensor,Tensor> batch_norm_cpu_transform_input_template(
    const Tensor& input, const Tensor& weight, const Tensor& bias,
    const Tensor& save_mean /* optional */, const Tensor& save_invstd /* optional */,
    const Tensor& running_mean /* optional */, const Tensor& running_var /* optional */,
    bool train, double eps) {

  // Check if we should use the fast path for contiguous memory format
  if (!train && input.is_contiguous()
      && (!weight.defined() || weight.is_contiguous())
      && (!bias.defined() || bias.is_contiguous())
      && running_mean.is_contiguous()
      && running_var.is_contiguous()) {

    Tensor output = at::empty_like(input, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
    batch_norm_cpu_inference_contiguous_stub(kCPU, output, input, weight,
        bias, running_mean, running_var, eps);
    return std::make_tuple(output, save_mean, save_invstd);
  }

  // Check if we should use the fast path for channel last memory format
  if (!train && input.is_contiguous(at::MemoryFormat::ChannelsLast)
      && (!weight.defined() || weight.is_contiguous())
      && (!bias.defined() || bias.is_contiguous())
      && running_mean.is_contiguous()
      && running_var.is_contiguous()) {

    Tensor output = at::empty_like(input, at::MemoryFormat::ChannelsLast);
    batch_norm_cpu_inference_channels_last<scalar_t>(
      output, input, weight, bias, running_mean, running_var, eps);
    return std::make_tuple(output, save_mean, save_invstd);
  }

  const int64_t ndim = input.dim();
  // Helper to convert 1d tensors to an nd tensor that broadcasts with input
  // All elements go into the channel dimension
  DimVector sizes(ndim, 1), strides(ndim, 0);
  auto as_nd = [&](const Tensor& t) {
    TORCH_INTERNAL_ASSERT(t.defined() && t.dim() == 1);
    sizes[1] = t.sizes()[0];
    strides[1] = t.strides()[0];
    return t.as_strided(sizes, strides);
  };

  auto mean = as_nd(train ? save_mean : running_mean);
  auto invstd = as_nd([&]{
    if (train) {
      return save_invstd;
    } else {
      return 1 / at::sqrt(running_var + eps);
    }
  }());
  auto w = weight.defined() ? as_nd(weight) :
      at::detail::scalar_tensor_static(1, input.scalar_type(), kCPU);
  auto b = bias.defined() ? as_nd(bias) :
      at::detail::scalar_tensor_static(0, input.scalar_type(), kCPU);

  Tensor output = at::empty(input.sizes(), input.options());
  auto iter = TensorIteratorConfig()
    .add_output(output)
    .add_input(input)
    .add_input(mean)
    .add_input(invstd)
    .add_input(w)
    .add_input(b)
    .build();

  cpu_kernel(iter, [=](scalar_t input, scalar_t mean, scalar_t invstd, scalar_t weight, scalar_t bias) {
    return ((input - mean) * invstd) * weight + bias;
  });
  return std::make_tuple(output, save_mean, save_invstd);
}

template<typename scalar_t, template<typename T> class VarTransform>
std::tuple<Tensor,Tensor> batch_norm_cpu_update_stats_template(
    const Tensor& input, const Tensor& running_mean, const Tensor& running_var,
    double momentum, double eps) {

  using accscalar_t = at::acc_type<scalar_t, false>;

  int64_t n_input = input.size(1);
  int64_t n = input.numel() / n_input;
  const int64_t ndim = input.dim();

  // Reduce all dimensions except dim=1
  DimVector reduce_dims(ndim - 1);
  reduce_dims[0] = 0;
  for (int64_t i = 2; i < ndim; ++i) {
    reduce_dims[i - 1] = i;
  }

  Tensor save_mean = at::mean(input, /*dims=*/reduce_dims);
  Tensor save_var_transform = at::empty({n_input}, input.options());
  auto save_mean_a = save_mean.accessor<scalar_t, 1>();
  auto save_var_transform_a = save_var_transform.accessor<scalar_t, 1>();

  auto running_mean_a = conditional_accessor_1d<scalar_t>(running_mean);
  auto running_var_a = conditional_accessor_1d<scalar_t>(running_var);

  parallel_for(0, n_input, 1, [&](int64_t b_begin, int64_t b_end) {
    for (int64_t f = b_begin; f < b_end; ++f) {
      Tensor in = input.select(1, f);

      // compute variance per input
      auto iter = TensorIteratorConfig()
        .add_input(in)
        .build();
      accscalar_t var_sum = 0;
      auto mean = static_cast<accscalar_t>(save_mean_a[f]);
      cpu_serial_kernel(iter, [&](const scalar_t i) -> void {
        var_sum += (i - mean) * (i - mean);
      });
      save_var_transform_a[f] = VarTransform<accscalar_t>{}(var_sum / n, eps);

      // update running averages
      if (running_mean.defined()) {
        running_mean_a[f] = momentum * mean + (1 - momentum) * running_mean_a[f];
      }
      if (running_var.defined()) {
        accscalar_t unbiased_var = var_sum / (n - 1);
        running_var_a[f] = momentum * unbiased_var + (1 - momentum) * running_var_a[f];
      }
    }
  });
  return std::make_tuple(save_mean, save_var_transform);
}


template<typename scalar_t>
std::tuple<Tensor, Tensor, Tensor> batch_norm_backward_cpu_template(const Tensor& grad_out_, const Tensor& input, const Tensor& weight,
                                                                    const Tensor& running_mean, const Tensor& running_var, const Tensor& save_mean, const Tensor& save_invstd,
                                                                    bool train, double eps, std::array<bool,3> grad_input_mask) {

  using accscalar_t = at::acc_type<scalar_t, false>;

  Tensor grad_input;
  Tensor grad_weight;
  Tensor grad_bias;
  if (grad_input_mask[0]) {
    grad_input = at::empty_like(input, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  }
  if (grad_input_mask[1]) {
    grad_weight = at::empty_like(weight, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  }
  if (grad_input_mask[2]) {
    grad_bias = at::empty_like(weight, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  }

  auto weight_a = conditional_accessor_1d<scalar_t>(weight);
  auto grad_weight_a = conditional_accessor_1d<scalar_t>(grad_weight);
  auto grad_bias_a = conditional_accessor_1d<scalar_t>(grad_bias);

  int64_t n_input = input.size(1);
  int64_t n = input.numel() / n_input;

  auto save_mean_a = conditional_accessor_1d<scalar_t>(save_mean);
  auto save_invstd_a = conditional_accessor_1d<scalar_t>(save_invstd);

  auto running_mean_a = conditional_accessor_1d<scalar_t>(running_mean);
  auto running_var_a = conditional_accessor_1d<scalar_t>(running_var);

  const int64_t ndim = input.dim();

  // Reduce all dimensions except dim=1
  DimVector reduce_dims(ndim - 1);
  reduce_dims[0] = 0;
  for (int64_t i = 2; i < ndim; ++i) {
    reduce_dims[i - 1] = i;
  }

  auto sum = at::sum(grad_out_, /*dims=*/reduce_dims);
  auto sum_a = sum.accessor<scalar_t, 1>();

  parallel_for(0, n_input, 1, [&](int64_t b_begin, int64_t b_end) {
      for (int64_t f = b_begin; f < b_end; ++f) {
        Tensor in = input.select(1, f);
        Tensor grad_out = grad_out_.select(1, f);

        scalar_t w = weight.defined() ? weight_a[f] : 1;

        scalar_t mean, invstd;
        if (train) {
          mean = save_mean_a[f];
          invstd = save_invstd_a[f];
        } else {
          mean = running_mean_a[f];
          invstd = 1 / std::sqrt(running_var_a[f] + eps);
        }

        // dot product of the Q(X) and gradOuput
        accscalar_t dotp = 0;
        auto iter = TensorIteratorConfig()
          .add_input(in)
          .add_input(grad_out)
          .build();
        cpu_serial_kernel(iter, [&](const scalar_t i, const scalar_t go) -> void {
          dotp += (i - mean) * go;
        });

        if (grad_input_mask[0]) {
          Tensor grad_in = grad_input.select(1, f);
          if (train) {
            // when in training mode
            // Q(X) = X - E[x] ; i.e. input centered to zero mean
            // Y = Q(X) / sigma    ; i.e. BN output before weight and bias
            // dL/dX = (Q(dL/dY) - dot(Y, dL/dY) * Y) / sigma * w

            // projection of gradOutput on to output scaled by std
            scalar_t k = (scalar_t) dotp * invstd * invstd / n;
            {
              auto iter = TensorIterator::unary_op(grad_in, in);
              cpu_serial_kernel(iter, [&](const scalar_t i) -> scalar_t {
                return (i - mean) * k;
              });
            }

            scalar_t grad_mean = sum_a[f] / n;
            {
              auto iter = TensorIterator::borrowing_binary_op(grad_in, grad_in, grad_out);
              cpu_serial_kernel(iter, [&](scalar_t gi, scalar_t go) -> scalar_t {
                return (go - grad_mean - gi) * invstd * w;
              });
            }
          } else {
            // when in evaluation mode
            // Q(X) = X - running_mean  ; i.e. input centered to zero mean
            // Y = Q(X) / running_std    ; i.e. BN output before weight and bias
            // dL/dX = w / running_std
            {
              auto iter = TensorIterator::unary_op(grad_in, grad_out);
              cpu_serial_kernel(iter, [&](const scalar_t i) -> scalar_t {
                return i * invstd * w;
              });
            }
          }
        }
        if (grad_input_mask[1]) {
          grad_weight_a[f] = dotp * invstd;
        }

        if (grad_input_mask[2]) {
          grad_bias_a[f] = sum_a[f];
        }
      }
    });
  return std::make_tuple(grad_input, grad_weight, grad_bias);
}

// _batch_norm_impl_index(_backward) are used in the JIT be able to keep the run-time selection
// of backends, while enabling it to keep the information about the used backend, so that it can
// use its corresponding backward implementation.
// XXX: The indices of backends need to be kept synchronized between this function and its _backward.
std::tuple<Tensor, Tensor, Tensor, Tensor, int64_t> _batch_norm_impl_index(
    const Tensor& input, const c10::optional<Tensor>& weight_opt /* optional */, const c10::optional<Tensor>& bias_opt /* optional */, const c10::optional<Tensor>& running_mean_opt /* optional */, const c10::optional<Tensor>& running_var_opt /* optional */,
    bool training, double momentum, double eps, bool cudnn_enabled) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] {return Tensor();});
  const Tensor& running_mean = c10::value_or_else(running_mean_opt, [] {return Tensor();});
  const Tensor& running_var = c10::value_or_else(running_var_opt, [] {return Tensor();});

  auto num_features = input.sizes()[1];
  if (running_mean.defined()) {
    check_dims_match_num_input_features("running_mean", num_features, running_mean.numel());
  } else if (!training) {
    AT_ERROR("running_mean must be defined in evaluation mode");
  }
  if (running_var.defined()) {
    check_dims_match_num_input_features("running_var", num_features, running_var.numel());
  } else if (!training) {
    AT_ERROR("running_var must be defined in evaluation mode");
  }
  if (weight.defined()) {
    check_dims_match_num_input_features("weight", num_features, weight.numel());
  }
  if (bias.defined()) {
    check_dims_match_num_input_features("bias", num_features, bias.numel());
  }

  const bool use_cudnn = (
      input.is_cuda()
      && input.scalar_type() != at::kBFloat16 && weight.scalar_type() != at::kBFloat16
      && (input.scalar_type() != at::kHalf
        || weight.scalar_type() == at::kFloat)
      && weight.defined() && bias.defined()
      && ((running_mean.defined() && running_var.defined())
        || (!running_mean.defined() && !running_var.defined() && training))
      && (input.dim() >= 3)
      && ((input.size(0) <= 880801 && training) // spatial, training
          ||(input.size(0) <= 65535 && !training)) //spatial, eval
      && detail::getCUDAHooks().compiledWithCuDNN()
      && eps >= detail::getCUDAHooks().batchnormMinEpsilonCuDNN()
      && cudnn_enabled && detail::getCUDAHooks().versionCuDNN() >= 5110L);

  if (use_cudnn) {
    auto input_c = input.contiguous(input.suggest_memory_format());
    auto weight_c = weight.contiguous();
    auto bias_c = bias.contiguous();
    auto rmean_c = running_mean.defined() ? running_mean.contiguous() : running_mean;
    auto rvar_c = running_var.defined() ? running_var.contiguous() : running_var;

    Tensor output, save_mean, save_var, reserve;
    std::tie(output, save_mean, save_var, reserve) =
        at::cudnn_batch_norm(input_c, weight_c, bias_c, rmean_c, rvar_c,
                             training, momentum, eps);

    return std::tuple<Tensor, Tensor, Tensor, Tensor, int64_t>(
        output, save_mean, save_var, reserve, 1);
  }

  Tensor reserve = at::empty({0}, input.options().dtype(kByte));

  bool use_miopen = (input.is_cuda()
               && input.dim() <= MIOPEN_DIM_MAX
               && input.scalar_type() != at::kDouble
               && input.scalar_type() != at::kBFloat16
               && (weight.scalar_type() != at::kHalf)
               && weight.defined() && bias.defined()
               && ((running_mean.defined() && running_var.defined())
                 || (!running_mean.defined() && !running_var.defined() && training))
               && detail::getCUDAHooks().compiledWithMIOpen()
               && cudnn_enabled
               );

  if (use_miopen) {
    return std::tuple_cat(
             at::miopen_batch_norm(
               input.contiguous(), weight.contiguous(), bias.contiguous(),
               running_mean.defined() ? running_mean.contiguous() : running_mean,
               running_var.defined() ? running_var.contiguous() : running_var,
               training, momentum, eps),
             std::tuple<Tensor>(reserve),
             std::make_tuple(2));
  }

  return std::tuple_cat(
           at::native_batch_norm(
             input, weight, bias, running_mean, running_var, training, momentum, eps),
           std::tuple<Tensor>(reserve),
           std::make_tuple(0));
}

std::tuple<Tensor, Tensor, Tensor> _batch_norm_impl_index_backward(
    int64_t impl_index,
    const Tensor& input, const Tensor& grad_output, const c10::optional<Tensor>& weight_opt /* optional */, const c10::optional<Tensor>& running_mean_opt /* optional */, const c10::optional<Tensor>& running_var_opt /* optional */, const c10::optional<Tensor>& save_mean_opt /* optional */, const c10::optional<Tensor>& save_var_transform_opt /* optional */,
    bool train, double epsilon, std::array<bool, 3> output_mask, const Tensor &reservedSpace) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& running_mean = c10::value_or_else(running_mean_opt, [] {return Tensor();});
  const Tensor& running_var = c10::value_or_else(running_var_opt, [] {return Tensor();});
  const Tensor& save_mean = c10::value_or_else(save_mean_opt, [] {return Tensor();});
  const Tensor& save_var_transform = c10::value_or_else(save_var_transform_opt, [] {return Tensor();});

  if (impl_index == 0) {
    return at::native_batch_norm_backward(grad_output, input, weight, running_mean, running_var, save_mean, save_var_transform, train, epsilon, output_mask);
  } else if (impl_index == 1) {
    // TODO: _batch_norm_impl_index_backward is only used in JIT. cudnn NHWC
    // format conversion is done inside cudnn_batch_norm_backward instead
    return at::cudnn_batch_norm_backward(input, grad_output, weight, running_mean, running_var, save_mean, save_var_transform, epsilon, reservedSpace);
  } else if (impl_index == 2) {
    return at::miopen_batch_norm_backward(input, grad_output, weight, running_mean, running_var, save_mean, save_var_transform, epsilon);
  }
  TORCH_INTERNAL_ASSERT(false, "Unsupported impl_index in _batch_norm_impl_index_backward: ", impl_index);
}

Tensor batch_norm(
    const Tensor& input, const c10::optional<Tensor>& weight_opt, const c10::optional<Tensor>& bias_opt,
    const c10::optional<Tensor>& running_mean_opt, const c10::optional<Tensor>& running_var_opt,
    bool training, double momentum, double eps, bool cudnn_enabled) {
  const Tensor& weight = c10::value_or_else(weight_opt, [] {return Tensor();});
  const Tensor& bias = c10::value_or_else(bias_opt, [] {return Tensor();});
  const Tensor& running_mean = c10::value_or_else(running_mean_opt, [] {return Tensor();});
  const Tensor& running_var = c10::value_or_else(running_var_opt, [] {return Tensor();});
  if (input.numel()==0){
    //don't return view of input, don't return empty tensor because it will break gradient chain
    auto out = input.clone();
    if (weight.defined()) out = out * weight[0];
    if (bias.defined()) out = out + bias[0];
    return out;
  }
  return std::get<0>(at::_batch_norm_impl_index(input, weight, bias, running_mean, running_var,
                                                training, momentum, eps, cudnn_enabled));
}

Tensor instance_norm(
    const Tensor& input, const c10::optional<Tensor>& weight_opt /* optional */, const c10::optional<Tensor>& bias_opt /* optional */, const c10::optional<Tensor>& running_mean_opt /* optional */, const c10::optional<Tensor>& running_var_opt /* optional */,
    bool use_input_stats, double momentum, double eps, bool cudnn_enabled) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] {return Tensor();});
  const Tensor& running_mean = c10::value_or_else(running_mean_opt, [] {return Tensor();});
  const Tensor& running_var = c10::value_or_else(running_var_opt, [] {return Tensor();});

  TORCH_CHECK(use_input_stats || (running_mean.defined() && running_var.defined()),
           "Expected running_mean and running_var to be defined when use_input_stats is false");
  std::vector<int64_t> shape = input.sizes().vec();
  int64_t b = input.size(0);
  int64_t c = input.size(1);
  shape[1] = b * c;
  shape[0] = 1;

  Tensor weight_ = repeat_if_defined(weight, b);
  Tensor bias_ = repeat_if_defined(bias, b);
  Tensor running_mean_ = repeat_if_defined(running_mean, b);
  Tensor running_var_ = repeat_if_defined(running_var, b);

  auto input_reshaped = input.contiguous().view(shape);
  auto out = at::batch_norm(input_reshaped, weight_, bias_, running_mean_, running_var_,
                            use_input_stats, momentum, eps, cudnn_enabled);

  // we alias running_mean and running_var because they are const but we want to modify their data
  if (running_mean.defined()) {
    at::alias(running_mean).copy_(running_mean_.view({ b, c }).mean(0, false));
  }
  if (running_var.defined()) {
    at::alias(running_var).copy_(running_var_.view({ b, c }).mean(0, false));
  }

  return out.view(input.sizes());
}

std::tuple<Tensor, Tensor> batch_norm_update_stats_cpu(
        const Tensor& self, const c10::optional<Tensor>& running_mean_opt, const c10::optional<Tensor>& running_var_opt, double momentum) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> running_mean_maybe_owned = at::borrow_from_optional_tensor(running_mean_opt);
  const Tensor& running_mean = *running_mean_maybe_owned;
  const Tensor& running_var = c10::value_or_else(running_var_opt, [] {return Tensor();});

  return AT_DISPATCH_FLOATING_TYPES(self.scalar_type(), "batch_norm_update_stats_cpu", [&] {
      return batch_norm_cpu_update_stats_template<scalar_t, Var>(self, running_mean, running_var, momentum, 0);
    });
}

std::tuple<Tensor, Tensor, Tensor> batch_norm_cpu(const Tensor& self, const c10::optional<Tensor>& weight_opt, const c10::optional<Tensor>& bias_opt, const c10::optional<Tensor>& running_mean_opt, const c10::optional<Tensor>& running_var_opt,
                                                  bool train, double momentum, double eps) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] {return Tensor();});
  const Tensor& running_mean = c10::value_or_else(running_mean_opt, [] {return Tensor();});
  const Tensor& running_var = c10::value_or_else(running_var_opt, [] {return Tensor();});

  checkBackend("batch_norm_cpu", {self, weight, bias, running_mean, running_var}, Backend::CPU);

  return AT_DISPATCH_FLOATING_TYPES(self.scalar_type(), "batch_norm", [&] {
      if (!train) {
        return batch_norm_cpu_transform_input_template<scalar_t>(self, weight, bias, {}, {}, running_mean, running_var, train, eps);
      } else {
        auto save_stats = batch_norm_cpu_update_stats_template<scalar_t, InvStd>(self, running_mean, running_var, momentum, eps);
        return batch_norm_cpu_transform_input_template<scalar_t>(self, weight, bias, std::get<0>(save_stats), std::get<1>(save_stats), running_mean, running_var, train, eps);
      }
    });
}

std::tuple<Tensor, Tensor, Tensor> batch_norm_backward_cpu(const Tensor& grad_out, const Tensor& self, const c10::optional<Tensor>& weight_opt, const c10::optional<Tensor>& running_mean_opt, const c10::optional<Tensor>& running_var_opt, const c10::optional<Tensor>& save_mean_opt, const c10::optional<Tensor>& save_invstd_opt,
                                                           bool train, double eps, std::array<bool,3> grad_input_mask) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& running_mean = c10::value_or_else(running_mean_opt, [] {return Tensor();});
  const Tensor& running_var = c10::value_or_else(running_var_opt, [] {return Tensor();});
  const Tensor& save_mean = c10::value_or_else(save_mean_opt, [] {return Tensor();});
  const Tensor& save_invstd = c10::value_or_else(save_invstd_opt, [] {return Tensor();});

  return AT_DISPATCH_FLOATING_TYPES(self.scalar_type(), "batch_norm_backward_cpu", [&] {
      return batch_norm_backward_cpu_template<scalar_t>(grad_out, self, weight, running_mean, running_var, save_mean, save_invstd, train, eps, grad_input_mask);
    });
}

TORCH_IMPL_FUNC(renorm_out)(const Tensor& self, const Scalar& p, int64_t dim,
                            const Scalar& maxnorm, const Tensor& out) {
  auto self_sizes = self.sizes();
  dim = c10::maybe_wrap_dim(dim, self_sizes.size());

  DimVector reduce_dims(self_sizes.size());
  std::iota(reduce_dims.begin(), reduce_dims.end(), 0);
  reduce_dims.erase(reduce_dims.begin() + dim);

  // For cuda half, calculate norm in float precision then cast
  // normalization factor to half
  auto dtype = self.scalar_type();
  auto acc_type = at::toAccumulateType(dtype, /*is_cuda=*/true);
  Tensor norm;
  if (acc_type != dtype) {
    norm = at::linalg_vector_norm(self, p.toDouble(), reduce_dims,
                                  /*keepdim=*/true, /*dtype=*/acc_type);
  } else {
    norm = at::linalg_vector_norm(self, p.toDouble(), reduce_dims,
                                  /*keepdim=*/true);
  }

  auto factor = (acc_type == c10::toValueType(dtype)) ?
      norm : at::empty(norm.sizes(), self.options());
  auto iter = TensorIteratorConfig()
      .add_output(factor)
      .add_input(norm)
      .set_check_mem_overlap(false)
      .cast_common_dtype_to_outputs(true)
      .build();

  renorm_scale_factor_stub(iter.device_type(), iter, maxnorm.toDouble());
  at::mul_outf(self, factor, const_cast<Tensor&>(out));
}

}} // at::native
