#include <ATen/ATen.h>
#include <ATen/Parallel.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/Pool.h>
#include <tuple>


namespace at {

namespace meta{
using namespace native;

TORCH_META_FUNC(avg_pool2d) (
  const Tensor& input,
  IntArrayRef kernel_size,
  IntArrayRef stride,
  IntArrayRef padding,
  bool ceil_mode,
  bool count_include_pad,
  c10::optional<int64_t> divisor_override
) {
  // #20866, #22032: Guarantee this for the official C++ API?
  TORCH_CHECK(kernel_size.size() == 1 || kernel_size.size() == 2,
    "avg_pool2d: kernel_size must either be a single int, or a tuple of two ints");
  const int kH = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kW = kernel_size.size() == 1 ? kH : safe_downcast<int, int64_t>(kernel_size[1]);

  TORCH_CHECK(stride.empty() || stride.size() == 1 || stride.size() == 2,
    "avg_pool2d: stride must either be omitted, a single int, or a tuple of two ints");
  const int dH = stride.empty() ? kH : safe_downcast<int, int64_t>(stride[0]);
  const int dW = stride.empty() ? kW :
                 stride.size() == 1 ? dH : safe_downcast<int, int64_t>(stride[1]);

  TORCH_CHECK(padding.size() == 1 || padding.size() == 2,
    "avg_pool2d: padding must either be a single int, or a tuple of two ints");
  const int padH = safe_downcast<int, int64_t>(padding[0]);
  const int padW = padding.size() == 1 ? padH : safe_downcast<int, int64_t>(padding[1]);

  TORCH_CHECK(!divisor_override.has_value() || divisor_override.value() != 0,
    "divisor must be not zero");

  /* sizes */
  const int64_t nbatch = input.ndimension() == 4 ? input.size(-4) : 1;
  const int64_t nInputPlane = input.size(-3);
  const int64_t inputHeight = input.size(-2);
  const int64_t inputWidth = input.size(-1);

  const int64_t outputHeight = pooling_output_shape<int64_t>(inputHeight, kH, padH, dH, 1, ceil_mode);
  const int64_t outputWidth = pooling_output_shape<int64_t>(inputWidth, kW, padW, dW, 1, ceil_mode);

  pool2d_shape_check(
    input,
    kH, kW, dH, dW, padH, padW, 1, 1,
    nInputPlane,
    inputHeight, inputWidth,
    outputHeight, outputWidth, input.suggest_memory_format());

  /* resize output */
  if (input.ndimension() == 3) {
    set_output(0, {nInputPlane, outputHeight, outputWidth}, input.options());
  }
  else {
    set_output(0, {nbatch, nInputPlane, outputHeight, outputWidth}, input.options());
  }
}

TORCH_META_FUNC(avg_pool2d_backward) (
  const Tensor& gradOutput_,
  const Tensor& input,
  IntArrayRef kernel_size,
  IntArrayRef stride,
  IntArrayRef padding,
  bool ceil_mode,
  bool count_include_pad,
  c10::optional<int64_t> divisor_override
) {
  // #20866, #22032: Guarantee this for the official C++ API?
  TORCH_CHECK(kernel_size.size() == 1 || kernel_size.size() == 2,
    "avg_pool2d: kernel_size must either be a single int, or a tuple of two ints");
  const int kH = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kW = kernel_size.size() == 1 ? kH : safe_downcast<int, int64_t>(kernel_size[1]);

  TORCH_CHECK(stride.empty() || stride.size() == 1 || stride.size() == 2,
    "avg_pool2d: stride must either be omitted, a single int, or a tuple of two ints");
  const int dH = stride.empty() ? kH : safe_downcast<int, int64_t>(stride[0]);
  const int dW = stride.empty() ? kW :
                 stride.size() == 1 ? dH : safe_downcast<int, int64_t>(stride[1]);

  TORCH_CHECK(padding.size() == 1 || padding.size() == 2,
    "avg_pool2d: padding must either be a single int, or a tuple of two ints");
  const int padH = safe_downcast<int, int64_t>(padding[0]);
  const int padW = padding.size() == 1 ? padH : safe_downcast<int, int64_t>(padding[1]);

  TORCH_CHECK(!divisor_override.has_value() || divisor_override.value() != 0, "divisor must be not zero");

  /* sizes */
  const int64_t nbatch = input.ndimension() == 4 ? input.size(-4) : 1;
  const int64_t nInputPlane = input.size(-3); // number of channels (or colors)
  const int64_t inputHeight = input.size(-2);
  const int64_t inputWidth = input.size(-1);
  const int64_t outputWidth = pooling_output_shape<int64_t>(inputWidth, kW, padW, dW, 1, ceil_mode);
  const int64_t outputHeight = pooling_output_shape<int64_t>(inputHeight, kH, padH, dH, 1, ceil_mode);

  avg_pool2d_backward_shape_check(
    input,
    gradOutput_,
    nbatch,
    kH, kW, dH, dW, padH, padW,
    nInputPlane,
    inputHeight, inputWidth,
    outputHeight, outputWidth,
    input.suggest_memory_format());

  /* resize output */
  set_output(0, input.sizes(), input.options());
}

} // namespace meta

namespace native {

namespace {

template <typename scalar_t>
static void avg_pool2d_out_frame(
          scalar_t *input_data,
          scalar_t *output_data,
          int64_t nbatch,
          int64_t nInputPlane,
          int64_t inputWidth,
          int64_t inputHeight,
          int64_t outputWidth,
          int64_t outputHeight,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH,
          bool count_include_pad,
          c10::optional<int64_t> divisor_override)
{
  at::parallel_for(0, nInputPlane, 0, [&](int64_t start, int64_t end) {
    for (auto k = start; k < end; k++)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
      int64_t p;
      for(p = 0; p < nbatch; p++)
      {
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        int64_t xx, yy;
        /* For all output pixels... */
        scalar_t *ptr_output = output_data + p*nInputPlane*outputWidth*outputHeight + k*outputWidth*outputHeight;
        const scalar_t *ptr_input = input_data + p*nInputPlane*inputWidth*inputHeight + k*inputWidth*inputHeight;
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        int64_t i;
        for(i = 0; i < outputWidth*outputHeight; i++)
          ptr_output[i] = 0;

        for(yy = 0; yy < outputHeight; yy++)
        {
          for(xx = 0; xx < outputWidth; xx++)
          {
            /* Compute the mean of the input image... */
            int64_t hstart = yy * dH - padH;
            int64_t wstart = xx * dW - padW;
            int64_t hend = std::min(hstart + kH, inputHeight + padH);
            int64_t wend = std::min(wstart + kW, inputWidth + padW);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
            int pool_size = (hend - hstart) * (wend - wstart);
            hstart = std::max(hstart, (int64_t) 0);
            wstart = std::max(wstart, (int64_t) 0);
            hend = std::min(hend, inputHeight);
            wend = std::min(wend, inputWidth);

            if (hstart >= hend || wstart >= wend) {
              ++ptr_output;
              continue;
            }

            scalar_t sum = 0;

            // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
            int divide_factor;
            if (divisor_override.has_value()) {
              divide_factor = divisor_override.value();
            } else {
              if(count_include_pad) {
                divide_factor = pool_size;
              } else {
                // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
                divide_factor = (hend - hstart) * (wend - wstart);
              }
            }

            // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
            int64_t kx, ky;

            for(ky = hstart; ky < hend; ky++)
            {
              for(kx = wstart; kx < wend; kx++)
                sum += ptr_input[ky*inputWidth + kx];
            }
            /* Update output */
            *ptr_output++ += sum/divide_factor;
          }
        }
      }
    }
  });
}

} // anonymous namespace

TORCH_IMPL_FUNC(avg_pool2d_out_cpu) (
  const Tensor &input_,
  IntArrayRef kernel_size,
  IntArrayRef stride,
  IntArrayRef padding,
  bool ceil_mode,
  bool count_include_pad,
  c10::optional<int64_t> divisor_override,
  const Tensor &output
) {
  const int kH = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kW = kernel_size.size() == 1 ? kH : safe_downcast<int, int64_t>(kernel_size[1]);

  const int dH = stride.empty() ? kH : safe_downcast<int, int64_t>(stride[0]);
  const int dW = stride.empty() ? kW :
                 stride.size() == 1 ? dH : safe_downcast<int, int64_t>(stride[1]);

  const int padH = safe_downcast<int, int64_t>(padding[0]);
  const int padW = padding.size() == 1 ? padH : safe_downcast<int, int64_t>(padding[1]);

  /* sizes */
  const int64_t nbatch = input_.ndimension() == 4 ? input_.size(-4) : 1;
  const int64_t nInputPlane = input_.size(-3);
  const int64_t inputHeight = input_.size(-2);
  const int64_t inputWidth = input_.size(-1);

  const int64_t outputHeight = pooling_output_shape<int64_t>(inputHeight, kH, padH, dH, 1, ceil_mode);
  const int64_t outputWidth = pooling_output_shape<int64_t>(inputWidth, kW, padW, dW, 1, ceil_mode);

  TORCH_CHECK(output.is_contiguous(), "avg_pool2d: output must be contiguous");

  Tensor input = input_.contiguous();

  AT_DISPATCH_FLOATING_TYPES_AND(at::ScalarType::Long, input.scalar_type(),
    "avg_pool2d_out_frame",
    [&] {
      scalar_t *input_data = input.data_ptr<scalar_t>();
      scalar_t *output_data = output.data_ptr<scalar_t>();

      avg_pool2d_out_frame(
        input_data,
        output_data,
        nbatch,
        nInputPlane,
        inputWidth, inputHeight,
        outputWidth, outputHeight,
        kW, kH,
        dW, dH,
        padW, padH,
        count_include_pad,
        divisor_override);
    }
  );
}

namespace {

template <typename scalar_t>
static void avg_pool2d_backward_out_frame(
          scalar_t *gradInput_data,
          scalar_t *gradOutput_data,
          int64_t nbatch,
          int64_t nInputPlane,
          int64_t inputWidth,
          int64_t inputHeight,
          int64_t outputWidth,
          int64_t outputHeight,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH,
          bool count_include_pad,
          c10::optional<int64_t> divisor_override)
{
  at::parallel_for(0, nInputPlane, 0, [&](int64_t start, int64_t end) {
    for (auto k = start; k < end; k++)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
      int64_t p;
      for(p = 0; p < nbatch; p++)
      {
        const scalar_t *ptr_gradOutput = gradOutput_data + p*nInputPlane*outputHeight*outputWidth + k*outputWidth*outputHeight;
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        int64_t xx, yy;

        scalar_t* ptr_gi = gradInput_data + p*nInputPlane*inputWidth*inputHeight + k*inputWidth*inputHeight;
        scalar_t *ptr_gradInput = gradInput_data + p*nInputPlane*inputWidth*inputHeight + k*inputWidth*inputHeight;

        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        int64_t i;
        for(i=0; i<inputWidth*inputHeight; i++)
          ptr_gi[i] = 0.0;

        for(yy = 0; yy < outputHeight; yy++)
        {
          for(xx = 0; xx < outputWidth; xx++)
          {
            int64_t hstart = yy * dH - padH;
            int64_t wstart = xx * dW - padW;
            int64_t hend = std::min(hstart + kH, inputHeight + padH);
            int64_t wend = std::min(wstart + kW, inputWidth + padW);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
            int pool_size = (hend - hstart) * (wend - wstart);
            hstart = std::max(hstart, (int64_t) 0);
            wstart = std::max(wstart, (int64_t) 0);
            hend = std::min(hend, inputHeight);
            wend = std::min(wend, inputWidth);

            scalar_t z = *ptr_gradOutput++;

            // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
            int divide_factor;
            if (divisor_override.has_value()) {
              divide_factor = divisor_override.value();
            } else {
              if(count_include_pad) {
                divide_factor = pool_size;
              } else {
                // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
                divide_factor = (hend - hstart) * (wend - wstart);
              }
            }

            // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
            int64_t kx, ky;
            for(ky = hstart ; ky < hend; ky++)
            {
              for(kx = wstart; kx < wend; kx++)
                ptr_gradInput[ky*inputWidth + kx] += z/divide_factor;
            }
          }
        }
      }
    }
  });
}

} // anonymous namespace

TORCH_IMPL_FUNC(avg_pool2d_backward_out_cpu) (
  const Tensor& gradOutput_,
  const Tensor& input,
  IntArrayRef kernel_size,
  IntArrayRef stride,
  IntArrayRef padding,
  bool ceil_mode,
  bool count_include_pad,
  c10::optional<int64_t> divisor_override,
  const Tensor& gradInput
) {
  const int kH = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kW = kernel_size.size() == 1 ? kH : safe_downcast<int, int64_t>(kernel_size[1]);

  const int dH = stride.empty() ? kH : safe_downcast<int, int64_t>(stride[0]);
  const int dW = stride.empty() ? kW :
                 stride.size() == 1 ? dH : safe_downcast<int, int64_t>(stride[1]);

  const int padH = safe_downcast<int, int64_t>(padding[0]);
  const int padW = padding.size() == 1 ? padH : safe_downcast<int, int64_t>(padding[1]);

  TORCH_CHECK(!divisor_override.has_value() || divisor_override.value() != 0, "divisor must be not zero");

  /* sizes */
  const int64_t nbatch = input.ndimension() == 4 ? input.size(-4) : 1;
  const int64_t nInputPlane = input.size(-3); // number of channels (or colors)
  const int64_t inputHeight = input.size(-2);
  const int64_t inputWidth = input.size(-1);
  const int64_t outputWidth = pooling_output_shape<int64_t>(inputWidth, kW, padW, dW, 1, ceil_mode);
  const int64_t outputHeight = pooling_output_shape<int64_t>(inputHeight, kH, padH, dH, 1, ceil_mode);

  /* get contiguous gradOutput */
  const Tensor gradOutput = gradOutput_.contiguous();

  TORCH_CHECK(gradInput.is_contiguous(), "gradInput must be contiguous");

  AT_DISPATCH_FLOATING_TYPES_AND(at::ScalarType::Long, input.scalar_type(),
    "avg_pool2d_backward_out_frame",
    [&] {
       scalar_t *gradInput_data = gradInput.data_ptr<scalar_t>();
       scalar_t *gradOutput_data = gradOutput.data_ptr<scalar_t>();

       avg_pool2d_backward_out_frame(
         gradInput_data,
         gradOutput_data,
         nbatch,
         nInputPlane,
         inputWidth, inputHeight,
         outputWidth, outputHeight,
         kW, kH,
         dW, dH,
         padW, padH,
         count_include_pad,
         divisor_override);
    }
  );
}

} // at::native
} // at
