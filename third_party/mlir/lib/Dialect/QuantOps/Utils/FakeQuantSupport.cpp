//===- FakeQuantSupport.cpp - Support utilities for FakeQuant ops ---------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "mlir/Dialect/QuantOps/FakeQuantSupport.h"
#include "mlir/Dialect/QuantOps/QuantTypes.h"

namespace mlir {
namespace quant {
namespace {
bool getDefaultStorageParams(unsigned numBits, bool narrowRange, bool isSigned,
                             MLIRContext *ctx, Type &storageType, int64_t &qmin,
                             int64_t &qmax) {
  // Hard-coded type mapping from TFLite.
  if (numBits <= 8) {
    storageType = IntegerType::get(8, ctx);
    if (isSigned) {
      qmin = -128;
      qmax = 127;
    } else {
      qmin = 0;
      qmax = 255;
    }
  } else if (numBits <= 16) {
    storageType = IntegerType::get(16, ctx);
    if (isSigned) {
      qmin = -32768;
      qmax = 32767;
    } else {
      qmin = 0;
      qmax = 65535;
    }
  } else {
    return true;
  }

  // Handle narrowRange.
  if (narrowRange) {
    qmin += 1;
  }
  return false;
}

void getScaleAndZeroPoint(int64_t qmin, int64_t qmax, double rmin, double rmax,
                          double &scale, int64_t &nudgedZeroPoint) {
  // Determine the scale.
  const double qminDouble = qmin;
  const double qmaxDouble = qmax;
  scale = (rmax - rmin) / (qmaxDouble - qminDouble);

  // Zero point computation.
  // In float, solve the affine equation for any known pair
  // (real value, corresponding quantized value), of which, two such pairs
  // are known: (rmin, qmin), (rmax, qmax).
  // The arithmetic error on the zero point computed from either pair will be
  // roughly machine_epsilon * (sum of absolute values of terms).
  // Use the variant that adds the smaller error.
  const double zeroPointFromMin = qminDouble - rmin / scale;
  const double zeroPointFromMinError =
      std::abs(qminDouble) + std::abs(rmin / scale);
  const double zeroPointFromMax = qmaxDouble - rmax / scale;
  const double zeroPointFromMaxError =
      std::abs(qmaxDouble) + std::abs(rmax / scale);

  const double zeroPointDouble = (zeroPointFromMinError < zeroPointFromMaxError)
                                     ? zeroPointFromMin
                                     : zeroPointFromMax;

  // Now nudge the zero point to be an integer.
  nudgedZeroPoint = 0;
  if (zeroPointDouble < qminDouble) {
    nudgedZeroPoint = qmin;
  } else if (zeroPointDouble > qmaxDouble) {
    nudgedZeroPoint = qmax;
  } else {
    nudgedZeroPoint = round(zeroPointDouble);
  }

  // By construction, the nudged zero point should always be in range.
  assert(nudgedZeroPoint >= qmin);
  assert(nudgedZeroPoint <= qmax);
}

} // end namespace

UniformQuantizedType fakeQuantAttrsToType(Location loc, unsigned numBits,
                                          double rmin, double rmax,
                                          bool narrowRange, Type expressedType,
                                          bool isSigned) {
  // Range must straddle zero.
  // TODO(b/140641593): remove this constraint.
  if (rmin > 0.0 || rmax < 0.0) {
    return (emitError(loc, "FakeQuant range must straddle zero: [")
                << rmin << "," << rmax << "]",
            nullptr);
  }

  MLIRContext *ctx = expressedType.getContext();
  unsigned flags = isSigned ? QuantizationFlags::Signed : 0;
  Type storageType;
  int64_t qmin;
  int64_t qmax;
  if (getDefaultStorageParams(numBits, narrowRange, isSigned, ctx, storageType,
                              qmin, qmax)) {
    return (emitError(loc, "unsupported FakeQuant number of bits: ") << numBits,
            nullptr);
  }

  // Special case where min/max is close enough. The tensor contents are all
  // 0.0s, so the scale is set to 1.0 and the tensor can be quantized to zero
  // points and dequantized to 0.0.
  if (std::fabs(rmax - rmin) < std::numeric_limits<double>::epsilon()) {
    return UniformQuantizedType::getChecked(flags, storageType, expressedType,
                                            1.0, qmin, qmin, qmax, loc);
  }

  double scale;
  int64_t nudgedZeroPoint;
  getScaleAndZeroPoint(qmin, qmax, rmin, rmax, scale, nudgedZeroPoint);

  return UniformQuantizedType::getChecked(flags, storageType, expressedType,
                                          scale, nudgedZeroPoint, qmin, qmax,
                                          loc);
}

// TODO(fengliuai): test this method once the quantizeAttr method is fixed.
UniformQuantizedPerAxisType
fakeQuantAttrsToType(Location loc, unsigned numBits, int32_t quantizedDimension,
                     ArrayRef<double> rmins, ArrayRef<double> rmaxs,
                     bool narrowRange, Type expressedType, bool isSigned) {
  size_t axis_size = rmins.size();
  if (axis_size != rmaxs.size()) {
    return (emitError(loc, "mismatched per-axis min and max size: ")
                << axis_size << " vs. " << rmaxs.size(),
            nullptr);
  }

  MLIRContext *ctx = expressedType.getContext();
  Type storageType;
  int64_t qmin;
  int64_t qmax;
  if (getDefaultStorageParams(numBits, narrowRange, isSigned, ctx, storageType,
                              qmin, qmax)) {
    return (emitError(loc, "unsupported FakeQuant number of bits: ") << numBits,
            nullptr);
  }

  SmallVector<double, 4> scales;
  SmallVector<int64_t, 4> zeroPoints;
  scales.reserve(axis_size);
  zeroPoints.reserve(axis_size);
  for (size_t axis = 0; axis != axis_size; ++axis) {
    double rmin = rmins[axis];
    double rmax = rmaxs[axis];
    if (std::fabs(rmax - rmin) < std::numeric_limits<double>::epsilon()) {
      scales.push_back(1.0);
      zeroPoints.push_back(qmin);
      continue;
    }

    double scale;
    int64_t nudgedZeroPoint;
    getScaleAndZeroPoint(qmin, qmax, rmin, rmax, scale, nudgedZeroPoint);
    scales.push_back(scale);
    zeroPoints.push_back(nudgedZeroPoint);
  }

  unsigned flags = isSigned ? QuantizationFlags::Signed : 0;
  return UniformQuantizedPerAxisType::getChecked(
      flags, storageType, expressedType, scales, zeroPoints, qmin, qmax,
      quantizedDimension, loc);
}

} // namespace quant
} // namespace mlir
