//===- TestDialect.cpp - MLIR Dialect for Testing -------------------------===//
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

#include "TestDialect.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Transforms/FoldUtils.h"
#include "mlir/Transforms/InliningUtils.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// TestDialect Interfaces
//===----------------------------------------------------------------------===//

namespace {
struct TestOpFolderDialectInterface : public OpFolderDialectInterface {
  using OpFolderDialectInterface::OpFolderDialectInterface;

  /// Registered hook to check if the given region, which is attached to an
  /// operation that is *not* isolated from above, should be used when
  /// materializing constants.
  bool shouldMaterializeInto(Region *region) const final {
    // If this is a one region operation, then insert into it.
    return isa<OneRegionOp>(region->getParentOp());
  }
};

/// This class defines the interface for handling inlining with standard
/// operations.
struct TestInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;

  //===--------------------------------------------------------------------===//
  // Analysis Hooks
  //===--------------------------------------------------------------------===//

  bool isLegalToInline(Operation *, Region *,
                       BlockAndValueMapping &) const final {
    return true;
  }

  bool shouldAnalyzeRecursively(Operation *op) const {
    // Analyze recursively if this is not a functional region operation, it
    // froms a separate functional scope.
    return !isa<FunctionalRegionOp>(op);
  }

  //===--------------------------------------------------------------------===//
  // Transformation Hooks
  //===--------------------------------------------------------------------===//

  /// Handle the given inlined terminator by replacing it with a new operation
  /// as necessary.
  void handleTerminator(Operation *op,
                        ArrayRef<Value *> valuesToRepl) const final {
    // Only handle "test.return" here.
    auto returnOp = dyn_cast<TestReturnOp>(op);
    if (!returnOp)
      return;

    // Replace the values directly with the return operands.
    assert(returnOp.getNumOperands() == valuesToRepl.size());
    for (const auto &it : llvm::enumerate(returnOp.getOperands()))
      valuesToRepl[it.index()]->replaceAllUsesWith(it.value());
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// TestDialect
//===----------------------------------------------------------------------===//

TestDialect::TestDialect(MLIRContext *context)
    : Dialect(getDialectName(), context) {
  addOperations<
#define GET_OP_LIST
#include "TestOps.cpp.inc"
      >();
  addInterfaces<TestOpFolderDialectInterface, TestInlinerInterface>();
  allowUnknownOperations();
}

//===----------------------------------------------------------------------===//
// Test IsolatedRegionOp - parse passthrough region arguments.
//===----------------------------------------------------------------------===//

static ParseResult parseIsolatedRegionOp(OpAsmParser *parser,
                                         OperationState *result) {
  OpAsmParser::OperandType argInfo;
  Type argType = parser->getBuilder().getIndexType();

  // Parse the input operand.
  if (parser->parseOperand(argInfo) ||
      parser->resolveOperand(argInfo, argType, result->operands))
    return failure();

  // Parse the body region, and reuse the operand info as the argument info.
  Region *body = result->addRegion();
  return parser->parseRegion(*body, argInfo, argType,
                             /*enableNameShadowing=*/true);
}

static void print(OpAsmPrinter *p, IsolatedRegionOp op) {
  *p << "test.isolated_region ";
  p->printOperand(op.getOperand());
  p->shadowRegionArgs(op.region(), op.getOperand());
  p->printRegion(op.region(), /*printEntryBlockArgs=*/false);
}

//===----------------------------------------------------------------------===//
// Test WrapRegionOp - wrapping op exercising `parseGenericOperation()`.
//===----------------------------------------------------------------------===//

static ParseResult parseWrappingRegionOp(OpAsmParser *parser,
                                         OperationState *result) {
  if (parser->parseOptionalKeyword("wraps"))
    return failure();

  // Parse the wrapped op in a region
  Region &body = *result->addRegion();
  body.push_back(new Block);
  Block &block = body.back();
  Operation *wrapped_op = parser->parseGenericOperation(&block, block.begin());
  if (!wrapped_op)
    return failure();

  // Create a return terminator in the inner region, pass as operand to the
  // terminator the returned values from the wrapped operation.
  SmallVector<Value *, 8> return_operands(wrapped_op->getResults());
  OpBuilder builder(parser->getBuilder().getContext());
  builder.setInsertionPointToEnd(&block);
  builder.create<TestReturnOp>(result->location, return_operands);

  // Get the results type for the wrapping op from the terminator operands.
  Operation &return_op = body.back().back();
  result->types.append(return_op.operand_type_begin(),
                       return_op.operand_type_end());
  return success();
}

static void print(OpAsmPrinter *p, WrappingRegionOp op) {
  *p << op.getOperationName() << " wraps ";
  p->printGenericOp(&op.region().front().front());
}

//===----------------------------------------------------------------------===//
// Test PolyForOp - parse list of region arguments.
//===----------------------------------------------------------------------===//
static ParseResult parsePolyForOp(OpAsmParser *parser, OperationState *result) {
  SmallVector<OpAsmParser::OperandType, 4> ivsInfo;
  // Parse list of region arguments without a delimiter.
  if (parser->parseRegionArgumentList(ivsInfo))
    return failure();

  // Parse the body region.
  Region *body = result->addRegion();
  auto &builder = parser->getBuilder();
  SmallVector<Type, 4> argTypes(ivsInfo.size(), builder.getIndexType());
  return parser->parseRegion(*body, ivsInfo, argTypes);
}

//===----------------------------------------------------------------------===//
// Test removing op with inner ops.
//===----------------------------------------------------------------------===//

namespace {
struct TestRemoveOpWithInnerOps
    : public OpRewritePattern<TestOpWithRegionPattern> {
  using OpRewritePattern<TestOpWithRegionPattern>::OpRewritePattern;

  PatternMatchResult matchAndRewrite(TestOpWithRegionPattern op,
                                     PatternRewriter &rewriter) const override {
    rewriter.replaceOp(op, llvm::None);
    return matchSuccess();
  }
};
} // end anonymous namespace

void TestOpWithRegionPattern::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<TestRemoveOpWithInnerOps>(context);
}

OpFoldResult TestOpWithRegionFold::fold(ArrayRef<Attribute> operands) {
  return operand();
}

// Static initialization for Test dialect registration.
static mlir::DialectRegistration<mlir::TestDialect> testDialect;

#define GET_OP_CLASSES
#include "TestOps.cpp.inc"
