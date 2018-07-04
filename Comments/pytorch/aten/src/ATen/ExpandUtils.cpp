#include "ATen/ExpandUtils.h"

namespace at {

std::vector<int64_t> infer_size(IntList a, IntList b) {
  auto dimsA = a.size();
  auto dimsB = b.size();
  ptrdiff_t ndim = dimsA > dimsB ? dimsA : dimsB;
  std::vector<int64_t> expandedSizes(ndim);

  for (long i = ndim - 1; i >= 0; --i) {
    long offset = ndim - 1 - i;
    long dimA = dimsA - 1 - offset;
    long dimB = dimsB - 1 - offset;
    long sizeA = (dimA >= 0) ? a[dimA] : 1;
    long sizeB = (dimB >= 0) ? b[dimB] : 1;

    AT_CHECK(
        sizeA == sizeB || sizeA == 1 || sizeB == 1,
        "The size of tensor a (", sizeA,
        ") must match the size of tensor b (", sizeB,
        ") at non-singleton dimension ", i);

    expandedSizes[i] = std::max(sizeA, sizeB);
  }

  return expandedSizes;
}

std::tuple<std::vector<int64_t>, std::vector<int64_t>> inferExpandGeometry(
    const Tensor& tensor,
    IntList sizes) {
  int64_t ndim = sizes.size();

  if (tensor.dim() == 0) {
    std::vector<int64_t> expandedStrides(ndim, 0);
    return std::tuple<std::vector<int64_t>, std::vector<int64_t>>(
        sizes.vec(), expandedStrides);
  }
  std::vector<int64_t> expandedSizes(ndim);
  std::vector<int64_t> expandedStrides(ndim);

  // create a new geometry for the tensors
  for (int64_t i = ndim - 1; i >= 0; --i) {
    int64_t offset = ndim - 1 - i;
    int64_t dim = tensor.dim() - 1 - offset;
    int64_t size = (dim >= 0) ? tensor.sizes()[dim] : 1;
    int64_t stride = (dim >= 0) ? tensor.strides()[dim]
                                : expandedSizes[i + 1] * expandedStrides[i + 1];
    int64_t targetSize = sizes[i];
    if (targetSize == -1) {
      AT_CHECK(
          dim >= 0,
          "The expanded size of the tensor (",
          targetSize,
          ") isn't allowed in a leading, non-existing dimension ",
          i);
      targetSize = size;
    }
    if (size != targetSize) {
      AT_CHECK(
          size == 1,
          "The expanded size of the tensor (",
          targetSize,
          ") must match the existing size (",
          size,
          ") at non-singleton dimension ",
          i);
      size = targetSize;
      stride = 0;
    }
    expandedSizes[i] = size;
    expandedStrides[i] = stride;
  }
  return std::tuple<std::vector<int64_t>, std::vector<int64_t>>(
      expandedSizes, expandedStrides);
}

} // namespace at
