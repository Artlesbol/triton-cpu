#include "ConvertDotCommon.h"

#include "cpu/include/TritonCPUTransforms/Passes.h"

#include "mlir/Dialect/AMX/AMXDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "include/triton/Analysis/Utility.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/TritonCPU/IR/Dialect.h"
#include <iostream>
#include <utility>

namespace mlir {
namespace triton {
namespace cpu {
#define GEN_PASS_DEF_CONVERTDOTTOAMX
#include "cpu/include/TritonCPUTransforms/Passes.h.inc"
} // namespace cpu
} // namespace triton
} // namespace mlir

using namespace mlir;
using namespace mlir::triton;
using namespace mlir::triton::cpu;

namespace {

// This structure is used to hold candidates for conversion to AMX
// Mul[F|I]Op operations.
struct AmxDotOpCandidate {
  // Operation to convert.
  cpu::DotOp op;
  // Available LHS, RHS, and accumulator types are limited in AMX and we might
  // require additional casts. Here we keep actual element types used by LHS,
  // RHS, and accumulator in AMX tiles.
  Type lhsTileElemTy;
  Type rhsTileElemTy;
  Type accTileElemTy;
  // AMX tile row size is limited by 64 bytes, so M and N dimensions are limited
  // by 16 because accumulator always has 4-byte elements. K dimension for tiles
  // is limited by 64 / <size of input element>. Here we keep actual tile sizes.
  int64_t tileM;
  int64_t tileN;
  int64_t tileK;
  // We have a limited number of available tiles, so if input/output is too
  // big to fit available tiles, we need to split them into blocks. Here we
  // keep a number of tiles in accumulator block. K dimension for input blocks
  // is always 1 tile now.
  int64_t tilesInBlockM;
  int64_t tilesInBlockN;
  // If accumulator is updated in a loop, then this flag indicates if we
  // should keep it in tiles the whole loop and move back to vectors only
  // after the loop.
  bool keepAccOnTiles = false;
  // If we want to keep accumulator in tiles but it's too big, then we might
  // keep it bufferized instead.
  bool keepAccInBuf = false;
  // If resulting tiles are not required to be trasfered to vectors and can be
  // directly stored to the output memory instead, then this field holds a
  // buffer to use.
  MemBuffer outBuf;
  // If output buffer is used then keep the original vector store here.
  Operation *origStore = nullptr;
};

// Check if input and output types can be handled by AMX (possibly, using
// additional casts for input/output). Returns true if AMX usage is possible.
// In this case, tile element type fields of the candidate structure are
// filled with actual types to be used in lowering.
bool checkElemTypes(Type lhsElemTy, Type rhsElemTy, Type accElemTy,
                    Type resElemTy, bool supportInt8, bool supportFp16,
                    bool supportBf16, AmxDotOpCandidate &candidate) {
  MLIRContext *ctx = lhsElemTy.getContext();
  if (lhsElemTy.isInteger()) {
    if (!supportInt8) {
      LDBG("Drop candidate because AMX_INT8 is not available.");
      return false;
    }

    // For integer case only i8 is allowed for LHS and RHS.
    if (!lhsElemTy.isInteger(8) || !rhsElemTy.isInteger(8)) {
      LDBG("Drop candidate with unsupported input integer type.");
      return false;
    }

    // Accumulator should be i32. If it's smaller, we will use casts.
    if (!accElemTy.isInteger() || accElemTy.getIntOrFloatBitWidth() > 32 ||
        !resElemTy.isInteger() || resElemTy.getIntOrFloatBitWidth() > 32) {
      LDBG("Drop candidate with unsupported output integer type.");
      return false;
    }

    candidate.lhsTileElemTy = IntegerType::get(ctx, 8);
    candidate.rhsTileElemTy = IntegerType::get(ctx, 8);
    candidate.accTileElemTy = IntegerType::get(ctx, 32);

    return true;
  }

  // FP case. Expect no integer args or result.
  if (rhsElemTy.isInteger() || accElemTy.isInteger() || resElemTy.isInteger()) {
    LDBG("Drop candidate with mixed int/fp types.");
    return false;
  }

  // For fp case LHS and RHS types should match and can be either FP16 or
  // BF16.
  if (lhsElemTy.getIntOrFloatBitWidth() > 16 ||
      rhsElemTy.getIntOrFloatBitWidth() > 16) {
    LDBG("Drop candidate with unsupported input fp type.");
    return false;
  }

  // Try to find a common input type. There is currently no support
  // for FP8 types, so promote them to FP16/BF16.
  Type commonInputElemTy;
  if (lhsElemTy.getIntOrFloatBitWidth() == 16) {
    commonInputElemTy = lhsElemTy;
    if (rhsElemTy.getIntOrFloatBitWidth() == 16 &&
        rhsElemTy != commonInputElemTy) {
      LDBG("Drop candidate with mismatched input types.");
      return false;
    }
  } else if (rhsElemTy.getIntOrFloatBitWidth() == 16)
    commonInputElemTy = rhsElemTy;
  // Both inputs are FP8, choose 16-bit FP type to use.
  else if (supportBf16)
    commonInputElemTy = BFloat16Type::get(ctx);
  else
    commonInputElemTy = Float16Type::get(ctx);

  if (commonInputElemTy.isF16() && !supportFp16) {
    LDBG("Drop candidate because AMX_FP16 is not available.");
    return false;
  }

  if (commonInputElemTy.isBF16() && !supportBf16) {
    LDBG("Drop candidate because AMX_BF16 is not available.");
    return false;
  }

  // Accumulator type should be FP32, we can use casts if it is smaller.
  if (accElemTy.getIntOrFloatBitWidth() > 32) {
    LDBG("Drop candidate with unsupported accumulator type.");
    return false;
  }

  candidate.lhsTileElemTy = commonInputElemTy;
  candidate.rhsTileElemTy = commonInputElemTy;
  candidate.accTileElemTy = Float32Type::get(ctx);

  return true;
}

// Check input shapes. Currently, support only 2D cases and ignore small
// inputs.
bool checkInputShapes(VectorType lhsTy, VectorType resTy) {
  if (lhsTy.getRank() != 2)
    return false;

  if (lhsTy.getDimSize(0) < 8 || lhsTy.getDimSize(1) < 8 ||
      resTy.getDimSize(1) < 8)
    return false;

  return true;
}

// Return a value that holds the resulting loop carried accumulator value.
// It's one of ForOp's results.
Value getResValueForLoopCarriedAcc(cpu::DotOp op) {
  Value updAcc = op.getResult();
  auto forOp = dyn_cast<scf::ForOp>(op->getParentOp());
  auto &use = *updAcc.getUses().begin();
  return forOp.getResult(use.getOperandNumber());
}

// Choose tile and block sizes for the candidate. Tile sizes are determined
// by input shapes and types. Block sizes are chosen to minimize number of
// tile loads/stores including tile register spills.
void setupBlockAndTileSizes(ArrayRef<int64_t> lhsShape,
                            ArrayRef<int64_t> resShape,
                            AmxDotOpCandidate &candidate) {
  int64_t m = resShape[0];
  int64_t n = resShape[1];
  int64_t k = lhsShape[1];
  int64_t tileM = std::min(m, (int64_t)16);
  int64_t tileN = std::min(n, (int64_t)16);
  int64_t tileK = std::min(
      k, (int64_t)512 / candidate.lhsTileElemTy.getIntOrFloatBitWidth());

  int64_t accBlocksM = m / tileM;
  int64_t accBlocksN = n / tileN;

  // All these sizes are power of 2. We have 8 tile registers and
  // cannot use them all for accumulator. So, we will use up to 4
  // tiles for accumulator in a single block.
  while (accBlocksM * accBlocksN > 4) {
    if (accBlocksM > accBlocksN)
      accBlocksM /= 2;
    else
      accBlocksN /= 2;
  }

  candidate.tileM = tileM;
  candidate.tileN = tileN;
  candidate.tileK = tileK;
  candidate.tilesInBlockM = accBlocksM;
  candidate.tilesInBlockN = accBlocksN;
}

// Check if a value is used only for a store and that this store can be
// replaced with tile stores. In this case fill appropriate fields in the
// candidate structure.
void findOutputBuffer(Value val, AmxDotOpCandidate &candidate) {
  if (val.hasOneUse()) {
    auto store = dyn_cast<vector::TransferWriteOp>(*val.user_begin());
    if (store && !hasMaskOrBoundsCheck(store))
      candidate.outBuf = MemBuffer{store.getSource(), store.getIndices()};
    candidate.origStore = store;
  }
}

// Check if specified ContractionOp can be lowered to AMX operations.
// If conversion is possible, then true is returned and candidate
// structure is filled with detailed transformation info.
bool isAmxCandidate(cpu::DotOp op, bool supportInt8, bool supportFp16,
                    bool supportBf16, AmxDotOpCandidate &candidate) {
  MLIRContext *ctx = op.getContext();
  VectorType lhsTy = cast<VectorType>(op.getA().getType());
  VectorType rhsTy = cast<VectorType>(op.getB().getType());
  VectorType accTy = cast<VectorType>(op.getC().getType());
  VectorType resTy = cast<VectorType>(op.getType());

  LDBG("Considering candidate op: " << op);

  // Check if input and output types match available hardware capabilities.
  // If check is successful then tile element types are filled with types
  // to use in AMX operations.
  if (!checkElemTypes(lhsTy.getElementType(), rhsTy.getElementType(),
                      accTy.getElementType(), resTy.getElementType(),
                      supportInt8, supportFp16, supportBf16, candidate))
    return false;

  // Check input shapes.
  if (!checkInputShapes(lhsTy, resTy))
    return false;

  candidate.op = op;
  setupBlockAndTileSizes(lhsTy.getShape(), resTy.getShape(), candidate);
  candidate.keepAccOnTiles = isLoopCarriedAcc(op.getC());

  // Can't keep acc in a tile the whole loop right now:
  // https://github.com/llvm/llvm-project/issues/109481
  if (candidate.keepAccOnTiles) {
    // We might not have enough tiles to hold the whole accumulator. If we
    // have more than one block, keep it in a bufffer.
    if (candidate.tilesInBlockM * candidate.tileM < resTy.getDimSize(0) ||
        candidate.tilesInBlockN * candidate.tileN < resTy.getDimSize(1)) {
      LDBG("Accumulator is too big to keep on tiles. Keep it bufferized "
           "insterad.");
      candidate.keepAccOnTiles = false;
      candidate.keepAccInBuf = true;
    } else {
      findOutputBuffer(getResValueForLoopCarriedAcc(op), candidate);
    }
  } else {
    findOutputBuffer(op.getResult(), candidate);
  }

  return true;
}

// In AMX, element values shoud be packed to 32-bit groups that would be
// multiplied elementwise with following accumulation. It means that RHS
// needs to be pre-packed. E.g. for the following input
//   B(0,0) B(0,1) B(0,2) ... B(0,15)
//   B(1,0) B(1,1) B(1,2) ... B(1,15)
//   B(2,0) B(2,1) B(2,2) ... B(2,15)
//   B(3,0) B(3,1) B(3,2) ... B(3,15)
// and BF16/FP16 type we need to transform it to
//   B(0,0) B(1,0) B(0,1), B(1,1) ... B(0,15) B(1,15)
//   B(2,0) B(3,0) B(2,1), B(3,1) ... B(2,15) B(3,15)
// so that original columns are 32-bits now. In case of int8 type, the
// result would be:
//   B(0,0) B(1,0) B(2,0), B(3,0) ... B(0,15) B(1,15), B(2,15) B(3,15)
void interleaveAndStore(Location loc, Value val, Value buf,
                        PatternRewriter &rewriter) {
  LDBG("Repacking operand before storing to a buffer.");
  VectorType valTy = cast<VectorType>(val.getType());
  int64_t rowsPerGroup = 32 / valTy.getElementTypeBitWidth();
  assert(rowsPerGroup == 2 || rowsPerGroup == 4);
  assert(valTy.getDimSize(0) % rowsPerGroup == 0);
  Value zeroIdx = index_cst(0);
  for (int64_t i = 0; i < valTy.getDimSize(0); i += rowsPerGroup) {
    Value row1, row2;
    if (rowsPerGroup == 2) {
      row1 = op_extract(val, i);
      row2 = op_extract(val, i + 1);
    } else {
      row1 = op_interleave(op_extract(val, i), op_extract(val, i + 2));
      row2 = op_interleave(op_extract(val, i + 1), op_extract(val, i + 3));
    }
    Value shuffled = op_interleave(row1, row2);
    Value idx = index_cst(i / rowsPerGroup);
    op_store(shuffled, buf, SmallVector<Value>({idx, zeroIdx}));
  }
}

Value loadWithPrefetch(Location loc, VectorType ty, Value memRef,
                       ArrayRef<Value> indices, ArrayRef<Value> step,
                       PatternRewriter &rewriter) {
  Value res = op_read(ty, memRef, indices);
  if (!step.empty()) {
    SmallVector<Value> prefetchIndices;
    for (int64_t i = 0; i < indices.size(); ++i) {
      prefetchIndices.push_back(
          op_addi(indices[i], rewriter.create<arith::IndexCastOp>(
                                  loc, rewriter.getIndexType(), step[i])));
    }
    rewriter.create<memref::PrefetchOp>(loc, memRef, prefetchIndices, false, 1,
                                        true);
  }
  return res;
}

// Copy tensor with packing using for-loop. See interleaveAndStore for more
// details.
void copyWithInterleave(Location loc, VectorType srcTy, const MemBuffer &src,
                        const MemBuffer &dst, PatternRewriter &rewriter) {
  int64_t rowsPerGroup = 32 / srcTy.getElementTypeBitWidth();
  Value lower = index_cst(0);
  Value upper = index_cst(srcTy.getDimSize(0) / rowsPerGroup);
  Value one = index_cst(1);
  Value rowsPerGroupVal = index_cst(rowsPerGroup);
  VectorType srcVecTy =
      VectorType::get({srcTy.getDimSize(1)}, srcTy.getElementType());
  auto forOp = rewriter.create<scf::ForOp>(loc, lower, upper, one);
  Value ivVal = forOp.getInductionVar();
  rewriter.setInsertionPointToStart(forOp.getBody());
  SmallVector<Value> srcIndices = src.indices;
  int64_t mDimIdx = srcIndices.size() - 2;
  Value scaledM = op_muli(ivVal, rowsPerGroupVal);
  srcIndices[mDimIdx] = op_addi(srcIndices[mDimIdx], scaledM);
  Value row1 = loadWithPrefetch(loc, srcVecTy, src.memRef, srcIndices, src.step,
                                rewriter);
  srcIndices[mDimIdx] = op_addi(srcIndices[mDimIdx], one);
  Value row2 = loadWithPrefetch(loc, srcVecTy, src.memRef, srcIndices, src.step,
                                rewriter);
  if (rowsPerGroup == 4) {
    srcIndices[mDimIdx] = op_addi(srcIndices[mDimIdx], one);
    Value row3 = loadWithPrefetch(loc, srcVecTy, src.memRef, srcIndices,
                                  src.step, rewriter);
    srcIndices[mDimIdx] = op_addi(srcIndices[mDimIdx], one);
    Value row4 = loadWithPrefetch(loc, srcVecTy, src.memRef, srcIndices,
                                  src.step, rewriter);
    row1 = op_interleave(row1, row3);
    row2 = op_interleave(row2, row4);
  }
  Value shuffled = op_interleave(row1, row2);
  SmallVector<Value> dstIndices = dst.indices;
  dstIndices[dstIndices.size() - 2] =
      op_addi(dstIndices[dstIndices.size() - 2], ivVal);
  op_write(shuffled, dst.memRef, dstIndices);
  rewriter.setInsertionPointAfter(forOp);
}

// Prepare temporary buffers to be used for tile loads. If the original
// value can be directly loaded to tiles from its original memory, then
// use it instead. Return empty buffer if source value is all zeros and
// skipForZeros is set.
//
// If interleave flag is set, then pre-pack RHS before store. See
// interleaveAndStore for more details.
MemBuffer prepareTensorBuffer(Location loc, Value val, bool interleave,
                              bool skipForZeros, bool readOnly,
                              Operation *allocaPoint,
                              PatternRewriter &rewriter) {
  LDBG("Preparing buffer (interleave=" << interleave
                                       << ") for a vector: " << val);
  auto vecTy = cast<VectorType>(val.getType());
  MemBuffer inputBuf = findInputBuffer(val, false, interleave);
  if (!inputBuf.empty()) {
    if (interleave && !inputBuf.vnni) {
      LDBG("  Copying from the original memref with interleave: "
           << inputBuf.memRef);
      auto tmpBuf = allocateTmpBufferStack(loc, getPackedLayoutType(vecTy),
                                           allocaPoint, rewriter);
      copyWithInterleave(loc, vecTy, inputBuf, tmpBuf, rewriter);
      return tmpBuf;
    }
    LDBG("  Reusing the original memref for a buffer: " << inputBuf.memRef);
    return inputBuf;
  }

  if (skipForZeros && isZeroConst(val)) {
    LDBG("Skip buffer for zero vector.");
    return {};
  }

  if (interleave)
    vecTy = getPackedLayoutType(vecTy);
  MemBuffer buf = allocateTmpBufferStack(loc, vecTy, allocaPoint, rewriter);

  if (interleave) {
    auto interleavedVal = getVnniSrc(val);
    if (interleavedVal) {
      LDBG("  Using pre-encoding value: " << interleavedVal);
      op_write(interleavedVal, buf.memRef, buf.indices);
    } else
      interleaveAndStore(loc, val, buf.memRef, rewriter);
  } else {
    op_write(val, buf.memRef, buf.indices);
  }

  return buf;
}

// Return a buffer where the final result should be stored. If result can
// be directly stored to the output memory, then it is used as an output
// buffer. Otherwise, re-use accumulator buffer or create a new one.
MemBuffer prepareResultBuffer(Location loc, Value val, const MemBuffer &accBuf,
                              const MemBuffer &outBuf, Operation *allocaPoint,
                              PatternRewriter &rewriter) {
  if (!outBuf.empty()) {
    LDBG("Output memory will be used for direct tile stores.");
    return outBuf;
  }

  if (!accBuf.empty()) {
    LDBG("Result will be stored to accumulator buffer.");
    return accBuf;
  }

  LDBG("Allocating buffer for the result.");
  return allocateTmpBufferStack(loc, cast<VectorType>(val.getType()),
                                allocaPoint, rewriter);
}

SmallVector<Value> shiftIndices(Location loc, ArrayRef<Value> indices,
                                amx::TileType tileTy, int64_t tilesInBlockM,
                                int64_t tilesInBlockN, int64_t blockM,
                                int64_t blockN, int64_t tileM, int64_t tileN,
                                PatternRewriter &rewriter) {
  int64_t blockOffsM = blockM * tilesInBlockM * tileTy.getDimSize(0);
  int64_t blockOffsN = blockN * tilesInBlockN * tileTy.getDimSize(1);
  int64_t tileOffsM = blockOffsM + tileM * tileTy.getDimSize(0);
  int64_t tileOffsN = blockOffsN + tileN * tileTy.getDimSize(1);
  SmallVector<Value> res(indices.begin(), indices.end() - 2);
  res.push_back(shiftIndex(loc, *(indices.end() - 2), tileOffsM, rewriter));
  res.push_back(shiftIndex(loc, *(indices.end() - 1), tileOffsN, rewriter));
  return res;
}

Value loadTile(Location loc, amx::TileType tileTy, const MemBuffer &buf,
               int64_t tilesInBlockM, int64_t tilesInBlockN, int64_t blockM,
               int64_t blockN, int64_t tileM, int64_t tileN,
               PatternRewriter &rewriter) {
  auto indices =
      shiftIndices(loc, buf.indices, tileTy, tilesInBlockM, tilesInBlockN,
                   blockM, blockN, tileM, tileN, rewriter);
  return rewriter.create<amx::TileLoadOp>(loc, tileTy, buf.memRef, indices);
}

void storeTile(Location loc, amx::TileType tileTy, Value val,
               const MemBuffer &buf, int64_t tilesInBlockM,
               int64_t tilesInBlockN, int64_t blockM, int64_t blockN,
               int64_t tileM, int64_t tileN, PatternRewriter &rewriter) {
  auto indices =
      shiftIndices(loc, buf.indices, tileTy, tilesInBlockM, tilesInBlockN,
                   blockM, blockN, tileM, tileN, rewriter);
  rewriter.create<amx::TileStoreOp>(loc, buf.memRef, indices, val);
}

SmallVector<SmallVector<Value>>
loadBlockTiles(Location loc, amx::TileType tileTy, const MemBuffer &buf,
               int64_t tilesInBlockM, int64_t tilesInBlockN, int64_t blockM,
               int64_t blockN, PatternRewriter &rewriter) {
  SmallVector<SmallVector<Value>> res(tilesInBlockM);
  for (int64_t m = 0; m < tilesInBlockM; ++m) {
    for (int64_t n = 0; n < tilesInBlockN; ++n) {
      Value tile = buf.memRef
                       ? loadTile(loc, tileTy, buf, tilesInBlockM,
                                  tilesInBlockN, blockM, blockN, m, n, rewriter)
                       : rewriter.create<amx::TileZeroOp>(loc, tileTy);
      res[m].push_back(tile);
    }
  }
  return res;
}

void storeBlockTiles(Location loc, amx::TileType tileTy, const MemBuffer &buf,
                     int64_t blockM, int64_t blockN,
                     const SmallVector<SmallVector<Value>> &tiles,
                     PatternRewriter &rewriter) {
  int64_t tilesInBlockM = tiles.size();
  int64_t tilesInBlockN = tiles[0].size();
  for (int64_t m = 0; m < tilesInBlockM; ++m) {
    for (int64_t n = 0; n < tilesInBlockN; ++n) {
      storeTile(loc, tileTy, tiles[m][n], buf, tilesInBlockM, tilesInBlockN,
                blockM, blockN, m, n, rewriter);
    }
  }
}

// Multiply two blocks. LHS block is preloaded to tiles with the following
// iteration over RHS. Accumulator values are updated in accTiles.
// Optionally, results can also be stored to accBuf.
void multiplyBlocksPreloadLhs(Location loc, amx::TileType lhsTileTy,
                              amx::TileType rhsTileTy, amx::TileType accTileTy,
                              const MemBuffer &lhsBuf, const MemBuffer &rhsBuf,
                              const MemBuffer &accBuf, int64_t blockM,
                              int64_t blockN, int64_t blockK,
                              int64_t tilesInBlockM, int64_t tilesInBlockN,
                              SmallVector<SmallVector<Value>> &accTiles,
                              bool storeResult, PatternRewriter &rewriter) {
  bool isInteger = accTileTy.getElementType().isInteger();
  SmallVector<SmallVector<Value>> lhsTiles = loadBlockTiles(
      loc, lhsTileTy, lhsBuf, tilesInBlockM, 1, blockM, blockK, rewriter);

  for (int64_t tileN = 0; tileN < tilesInBlockN; ++tileN) {
    Value rhsTile = loadTile(loc, rhsTileTy, rhsBuf, 1, tilesInBlockN, blockK,
                             blockN, 0, tileN, rewriter);

    for (int64_t tileM = 0; tileM < tilesInBlockM; ++tileM) {
      if (isInteger)
        accTiles[tileM][tileN] =
            rewriter.create<amx::TileMulIOp>(loc, accTileTy, lhsTiles[tileM][0],
                                             rhsTile, accTiles[tileM][tileN]);
      else
        accTiles[tileM][tileN] =
            rewriter.create<amx::TileMulFOp>(loc, accTileTy, lhsTiles[tileM][0],
                                             rhsTile, accTiles[tileM][tileN]);

      // Insert store here to better mix stores with multiplications.
      if (storeResult) {
        storeTile(loc, accTileTy, accTiles[tileM][tileN], accBuf, tilesInBlockM,
                  tilesInBlockN, blockM, blockN, tileM, tileN, rewriter);
      }
    }
  }
}

// Similar to multiplyBlocksPreloadLhs but here RHS is preloaded to tiles.
void multiplyBlocksPreloadRhs(Location loc, amx::TileType lhsTileTy,
                              amx::TileType rhsTileTy, amx::TileType accTileTy,
                              const MemBuffer &lhsBuf, const MemBuffer &rhsBuf,
                              const MemBuffer &accBuf, int64_t blockM,
                              int64_t blockN, int64_t blockK,
                              int64_t tilesInBlockM, int64_t tilesInBlockN,
                              SmallVector<SmallVector<Value>> &accTiles,
                              bool storeResult, PatternRewriter &rewriter) {
  bool isInteger = accTileTy.getElementType().isInteger();
  SmallVector<SmallVector<Value>> rhsTiles = loadBlockTiles(
      loc, rhsTileTy, rhsBuf, 1, tilesInBlockN, blockK, blockN, rewriter);

  for (int64_t tileM = 0; tileM < tilesInBlockM; ++tileM) {
    Value lhsTile = loadTile(loc, lhsTileTy, lhsBuf, tilesInBlockM, 1, blockM,
                             blockK, tileM, 0, rewriter);

    for (int64_t tileN = 0; tileN < tilesInBlockN; ++tileN) {
      if (isInteger)
        accTiles[tileM][tileN] = rewriter.create<amx::TileMulIOp>(
            loc, accTileTy, lhsTile, rhsTiles[0][tileN],
            accTiles[tileM][tileN]);
      else
        accTiles[tileM][tileN] = rewriter.create<amx::TileMulFOp>(
            loc, accTileTy, lhsTile, rhsTiles[0][tileN],
            accTiles[tileM][tileN]);

      // Insert store here to better mix stores with multiplications.
      if (storeResult) {
        storeTile(loc, accTileTy, accTiles[tileM][tileN], accBuf, tilesInBlockM,
                  tilesInBlockN, blockM, blockN, tileM, tileN, rewriter);
      }
    }
  }
}

LogicalResult convertCandidate(AmxDotOpCandidate &candidate,
                               PatternRewriter &rewriter) {
  cpu::DotOp op = candidate.op;
  Location loc = op.getLoc();
  VectorType lhsTy = cast<VectorType>(op.getA().getType());
  VectorType rhsTy = cast<VectorType>(op.getB().getType());
  VectorType accTy = cast<VectorType>(op.getC().getType());
  VectorType resTy = cast<VectorType>(op.getResult().getType());
  amx::TileType lhsTileTy = amx::TileType::get(
      SmallVector<int64_t>({candidate.tileM, candidate.tileK}),
      candidate.lhsTileElemTy);
  amx::TileType rhsTileTy = getPackedLayoutType(amx::TileType::get(
      SmallVector<int64_t>({candidate.tileK, candidate.tileN}),
      candidate.rhsTileElemTy));
  amx::TileType accTileTy = amx::TileType::get(
      SmallVector<int64_t>({candidate.tileM, candidate.tileN}),
      candidate.accTileElemTy);

  // If we don't work with a loop and want to directly store tiles into output
  // memory, then use the original store as insertion point to have its buffer
  // values available for generated code.
  if (!candidate.keepAccInBuf && !candidate.keepAccOnTiles &&
      !candidate.outBuf.empty())
    rewriter.setInsertionPoint(candidate.origStore);

  Operation *allocaPoint = op;
  while (!isa<triton::FuncOp>(allocaPoint->getParentOp()))
    allocaPoint = allocaPoint->getParentOp();

  // Cast input data if required and prepare input buffer. It might be temporary
  // buffers with stored vectors or the original input memory.
  Value lhs = maybeCast(loc, op.getA(), candidate.lhsTileElemTy, rewriter);
  MemBuffer lhsBuf =
      prepareTensorBuffer(loc, lhs, false, false, true, allocaPoint, rewriter);

  Value rhs = maybeCast(loc, op.getB(), candidate.rhsTileElemTy, rewriter);
  MemBuffer rhsBuf =
      prepareTensorBuffer(loc, rhs, true, false, true, allocaPoint, rewriter);

  Value acc = maybeCast(loc, op.getC(), candidate.accTileElemTy, rewriter);
  Value accToStore = acc;
  scf::ForOp forOp;
  if (candidate.keepAccInBuf || candidate.keepAccOnTiles) {
    forOp = cast<scf::ForOp>(op->getParentOp());
    accToStore = getInitAccValue(acc);
  }
  MemBuffer accBuf;
  {
    // If accumulator is bufferized then we should move initial values before
    // the loop.
    OpBuilder::InsertionGuard g(rewriter);
    if (candidate.keepAccInBuf)
      rewriter.setInsertionPoint(forOp);
    accBuf =
        prepareTensorBuffer(loc, accToStore, false, !candidate.keepAccInBuf,
                            false, allocaPoint, rewriter);
  }

  MemBuffer resBuf = prepareResultBuffer(
      loc, op.getResult(), accBuf, candidate.outBuf, allocaPoint, rewriter);

  SmallVector<SmallVector<Value>> accTiles;
  SmallVector<SmallVector<Value>> accInitTiles;
  if (candidate.keepAccOnTiles) {
    // Initial tile values are loaded before the loop and then directly
    // used within the loop. Later, new iter values will be added to
    // add loop carried-dependencies for accumulator tiles and accInitTiles
    // will be used as initializers for them.
    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPoint(forOp);
    LDBG("Loading accumulator to tiles before the loop.");
    accInitTiles =
        loadBlockTiles(loc, accTileTy, accBuf, candidate.tilesInBlockM,
                       candidate.tilesInBlockN, 0, 0, rewriter);
    accTiles = accInitTiles;
  }

  int64_t blocksInAccM =
      accTy.getDimSize(0) / candidate.tileM / candidate.tilesInBlockM;
  int64_t blocksInAccN =
      accTy.getDimSize(1) / candidate.tileN / candidate.tilesInBlockN;
  int64_t tilesInVectorK = lhsTy.getDimSize(1) / candidate.tileK;
  for (int64_t blockM = 0; blockM < blocksInAccM; ++blockM) {
    for (int64_t blockN = 0; blockN < blocksInAccN; ++blockN) {
      if (!candidate.keepAccOnTiles)
        accTiles =
            loadBlockTiles(loc, accTileTy, accBuf, candidate.tilesInBlockM,
                           candidate.tilesInBlockN, blockM, blockN, rewriter);

      for (int64_t blocK = 0; blocK < tilesInVectorK; ++blocK) {
        // We can store accumulator if it is the last block over K dimension.
        // TODO: enable forward store for acc kept in tiles.
        bool storeAcc =
            !candidate.keepAccOnTiles && (blocK == (tilesInVectorK - 1));

        // We need to choose which block (LHS or RHS) to keep on tiles.
        // E.g. for ACC block 4x1 tiles, LHS block is also 4 tiles, so
        // we would use all tile registers trying to keep both ACC and
        // LHS blocks on registers. To decrease register pressure, keep
        // the smallest block on tiles.
        if (candidate.tilesInBlockM <= candidate.tilesInBlockN)
          multiplyBlocksPreloadLhs(
              loc, lhsTileTy, rhsTileTy, accTileTy, lhsBuf, rhsBuf, resBuf,
              blockM, blockN, blocK, candidate.tilesInBlockM,
              candidate.tilesInBlockN, accTiles, storeAcc, rewriter);
        else
          multiplyBlocksPreloadRhs(
              loc, lhsTileTy, rhsTileTy, accTileTy, lhsBuf, rhsBuf, resBuf,
              blockM, blockN, blocK, candidate.tilesInBlockM,
              candidate.tilesInBlockN, accTiles, storeAcc, rewriter);
      }
    }
  }

  if (candidate.keepAccOnTiles) {
    // In this case we have the whole accumulator/result on tiles. Loop
    // carried dependencies are not in place yet and should be added.
    // After the loop, resulting tiles should either be stored to the
    // output buffer, or moved to a vector though a temporary buffer.

    // We don't need the original accumulator and contraction op anymore.
    // Directly yield orig accumulator value, so it would be later removed
    // as unused. The original contraction can be removed right away.
    int64_t origResIdx = op.getResult().getUses().begin()->getOperandNumber();
    rewriter.replaceOp(op, op.getC());

    // Now, replace the loop with a new one to add loop carried dependency for
    // accumulator tiles.
    LDBG("Rewrite loop to introduce loop carried dependencies for accumulator "
         "tiles.");
    SmallVector<Value> newInitOperands;
    SmallVector<Value> newYieldedValues;
    for (int64_t m = 0; m < candidate.tilesInBlockM; ++m)
      for (int64_t n = 0; n < candidate.tilesInBlockN; ++n) {
        LDBG("Initial value\n  " << accInitTiles[m][n]
                                 << "\nis combined with\n  " << accTiles[m][n]);
        newInitOperands.push_back(accInitTiles[m][n]);
        newYieldedValues.push_back(accTiles[m][n]);
      }
    auto newForOp = cast<scf::ForOp>(*forOp.replaceWithAdditionalYields(
        rewriter, newInitOperands, true,
        [&newYieldedValues](OpBuilder &b, Location loc,
                            ArrayRef<BlockArgument> newBBArgs) {
          return newYieldedValues;
        }));

    // The resulting tiles are now in the new loop results.
    auto resTiles = newForOp.getResults().take_back(newYieldedValues.size());
    for (int64_t m = 0; m < candidate.tilesInBlockM; ++m)
      for (int64_t n = 0; n < candidate.tilesInBlockN; ++n) {
        accTiles[m][n] = resTiles[m * candidate.tilesInBlockN + n];
      }

    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPointAfter(newForOp);
    if (candidate.outBuf.empty()) {
      // Move tiles to a vector through a temporary buffer and use it instead
      // of the original one.
      LDBG("Moving resulting tiles to a vector through memory.");
      VectorType resTy = accTy.cloneWith(std::nullopt, candidate.accTileElemTy);
      storeBlockTiles(loc, accTileTy, resBuf, 0, 0, accTiles, rewriter);
      Value newVal = op_read(resTy, resBuf.memRef, resBuf.indices);
      // We might need to cast back to the original type.
      newVal = maybeCast(loc, newVal, accTy.getElementType(), rewriter);
      rewriter.replaceAllUsesWith(newForOp.getResult(origResIdx), newVal);
    } else {
      // Store tiles directly to the output buffer and remove the original
      // store.
      LDBG("Storing  resulting tiles to the output memory.");
      OpBuilder::InsertionGuard g(rewriter);
      rewriter.setInsertionPoint(candidate.origStore);
      storeBlockTiles(loc, accTileTy, candidate.outBuf, 0, 0, accTiles,
                      rewriter);
      rewriter.eraseOp(candidate.origStore);
    }
  } else if (candidate.keepAccInBuf) {
    // The result is in the buffer. We should load it and replace one of the
    // loop results. The original contraction op can be removed.
    // TODO: should we try to store to the output buffer on the last iteration?
    Value loopRes = forOp.getTiedLoopResult(cast<BlockArgument>(op.getC()));
    LDBG(
        "Loading buffererized accumulator to a vector to replace loop result.");
    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPointAfter(forOp);
    Value newVal =
        op_read(cast<VectorType>(acc.getType()), resBuf.memRef, resBuf.indices);
    // We might need to cast back to the original type.
    newVal = maybeCast(loc, newVal, accTy.getElementType(), rewriter);
    rewriter.replaceAllUsesWith(loopRes, newVal);
    // Directly yield orig accumulator iter value. It will be removed as unused
    // later.
    rewriter.replaceOp(op, op.getC());
  } else if (candidate.outBuf.empty()) {
    // The result is in the buffer. We should load it and replace the original
    // constraction result.
    LDBG("Loading the result to a vector to replace orig op result.");
    Value newVal =
        op_read(cast<VectorType>(acc.getType()), resBuf.memRef, resBuf.indices);
    // We might need to cast back to the original type.
    newVal = maybeCast(loc, newVal, accTy.getElementType(), rewriter);
    rewriter.replaceOp(op, newVal);
  } else {
    // The result is already in the output buffer. We just need to remove the
    // original contraction and store operation.
    LDBG("Removing original operation and its use.");
    rewriter.eraseOp(candidate.origStore);
    rewriter.eraseOp(op);
  }

  return success();
}

struct ConvertDotToAMX
    : public triton::cpu::impl::ConvertDotToAMXBase<ConvertDotToAMX> {
  ConvertDotToAMX() = default;
  ConvertDotToAMX(bool convertInt8, bool convertFp16, bool convertBf16) {
    this->convertInt8 = convertInt8;
    this->convertFp16 = convertFp16;
    this->convertBf16 = convertBf16;
  }

  void runOnOperation() override {
    if (!convertInt8 && !convertFp16 && !convertBf16)
      return;

    MLIRContext *context = &getContext();
    ModuleOp mod = getOperation();

    SmallVector<AmxDotOpCandidate> candidates;
    mod->walk([this, &candidates](cpu::DotOp op) {
      AmxDotOpCandidate candidate;
      if (isAmxCandidate(op, convertInt8, convertFp16, convertBf16,
                         candidate)) {
        LLVM_DEBUG({
          LDBG("Found AMX candidate");
          LDBG("  Op: " << candidate.op);
          LDBG("  LhsTileElemTy: " << candidate.lhsTileElemTy);
          LDBG("  RhsTileElemTy: " << candidate.rhsTileElemTy);
          LDBG("  AccTileElemTy: " << candidate.accTileElemTy);
          LDBG("  TileM: " << candidate.tileM);
          LDBG("  TileN: " << candidate.tileN);
          LDBG("  TileK: " << candidate.tileK);
          LDBG("  TilesInBlockM: " << candidate.tilesInBlockM);
          LDBG("  TilesInBlockN: " << candidate.tilesInBlockN);
          LDBG("  KeepAccOnTiles: " << candidate.keepAccOnTiles);
          LDBG("  KeepAccInBuf: " << candidate.keepAccInBuf);
          LDBG("  Has output buffer: " << !candidate.outBuf.empty());
        });
        candidates.push_back(candidate);
      }
      return WalkResult::advance();
    });

    for (auto &candidate : candidates) {
      LDBG("Starting conversion of candidate: " << candidate.op);
      PatternRewriter rewriter(context);
      rewriter.setInsertionPoint(candidate.op);
      if (succeeded(convertCandidate(candidate, rewriter))) {
        LDBG("Conversion succeeded!");
      } else {
        LDBG("Conversion failed!");
      }
    }
  }
};

} // namespace

namespace mlir {
namespace triton {
namespace cpu {

std::unique_ptr<OperationPass<ModuleOp>> createConvertDotToAMX() {
  return std::make_unique<ConvertDotToAMX>();
}

std::unique_ptr<OperationPass<ModuleOp>>
createConvertDotToAMX(bool convertInt8, bool convertFp16, bool convertBf16) {
  return std::make_unique<ConvertDotToAMX>(convertInt8, convertFp16,
                                           convertBf16);
}

} // namespace cpu
} // namespace triton
} // namespace mlir
