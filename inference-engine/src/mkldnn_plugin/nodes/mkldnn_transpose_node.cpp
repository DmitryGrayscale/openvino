// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_transpose_node.h"

#include <algorithm>
#include <string>
#include <mkldnn_extension_utils.h>
#include <mkldnn_selective_build.h>
#include "ie_parallel.hpp"
#include "utils/bfloat16.hpp"
#include <utils/general_utils.h>
#include "utils/ngraph_utils.hpp"

using namespace mkldnn;
using namespace MKLDNNPlugin;
using namespace InferenceEngine;


bool MKLDNNTransposeNode::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        if (isDynamicNgraphNode(op)) {
            errorMessage = "Doesn't support op with dynamic shapes";
            return false;
        }

        const auto transposeOp = ngraph::as_type_ptr<const ngraph::op::v1::Transpose>(op);
        if (!transposeOp) {
            errorMessage = "Node is not an instance of the Transpose operation.";
            return false;
        }

        auto orderOp = ngraph::as_type_ptr<ngraph::op::Constant>(op->get_input_node_shared_ptr(1));
        if (!orderOp) {
            errorMessage = "Constant expected as the second input.";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

MKLDNNTransposeNode::MKLDNNTransposeNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng, MKLDNNWeightsSharing::Ptr &cache)
        : MKLDNNNode(op, eng, cache) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        IE_THROW(NotImplemented) << errorMessage;
    }

    auto orderOp = ngraph::as_type_ptr<ngraph::op::Constant>(op->get_input_node_shared_ptr(1));
    order = orderOp->cast_vector<size_t>();

    if (order.empty()) {
        size_t rank = op->get_input_shape(0).size();
        for (size_t i = 1lu; i <= rank; ++i) {
            order.emplace_back(rank - i);
        }
    }
}

void MKLDNNTransposeNode::getSupportedDescriptors() {
}

void MKLDNNTransposeNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    prec = getOriginalInputPrecisionAtPort(0);

    auto& creatorsMap = BlockedDescCreator::getCommonCreators();

    NodeConfig config;
    config.dynBatchSupport = true;
    config.inConfs.resize(2);
    config.outConfs.resize(1);
    config.inConfs[0].inPlace = -1;
    config.inConfs[0].constant = false;
    config.outConfs[0].inPlace = -1;
    config.outConfs[0].constant = false;
    config.inConfs[1].desc = creatorsMap.at(LayoutType::ncsp)->createSharedDesc(getOriginalInputPrecisionAtPort(1), getInputShapeAtPort(1));

    if (getInputShapeAtPort(0).getRank() == 4 || getInputShapeAtPort(0).getRank() == 5) {
        config.inConfs[0].desc = creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, getInputShapeAtPort(0));
        config.outConfs[0].desc = creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, getOutputShapeAtPort(0));
        supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});

        auto srcDims = getInputShapeAtPort(0).getStaticDims();
        if (srcDims[1] % 8 == 0) {
            config.inConfs[0].desc = creatorsMap.at(LayoutType::nCsp8c)->createSharedDesc(prec, getInputShapeAtPort(0));
            supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
        }

        if (srcDims[1] % 16 == 0) {
            config.inConfs[0].desc = creatorsMap.at(LayoutType::nCsp16c)->createSharedDesc(prec, getInputShapeAtPort(0));
            supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
        }

        if (prec == Precision::FP32 || prec == Precision::I8 || prec == Precision::U8) {
            config.inConfs[0].desc = creatorsMap.at(LayoutType::nspc)->createSharedDesc(prec, getInputShapeAtPort(0));
            config.outConfs[0].desc = creatorsMap.at(LayoutType::nspc)->createSharedDesc(prec, getOutputShapeAtPort(0));
            supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
        }
    } else {
        // general plain case
        config.inConfs[0].desc = creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, getInputShapeAtPort(0));
        config.outConfs[0].desc = creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, getOutputShapeAtPort(0));
        supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
    }
}

void MKLDNNTransposeNode::createPrimitive() {
    auto& dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto& srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    if (!dstMemPtr || !dstMemPtr->GetPrimitivePtr())
        IE_THROW() << "Destination memory didn't allocate.";
    if (!srcMemPtr || !srcMemPtr->GetPrimitivePtr())
        IE_THROW() << "Input memory didn't allocate.";
    if (getSelectedPrimitiveDescriptor() == nullptr)
        IE_THROW() << "Preferable primitive descriptor is not set.";

    if (getParentEdgeAt(0)->getMemory().getDesc().hasLayoutType(LayoutType::ncsp) &&
        std::find(optimizedOrders.begin(), optimizedOrders.end(), order) != optimizedOrders.end()) {
        isOptimized = true;
        return;
    }

    PermuteParams params;
    params.data_size = getSelectedPrimitiveDescriptor()->getConfig().inConfs[0].desc->getPrecision().size();
    params.order = order;
    auto srcDesc = getParentEdgeAt(0)->getMemory().GetDescWithType<BlockedMemoryDesc>();
    params.src_block_dims = srcDesc->getBlockDims();
    params.src_block_order = srcDesc->getOrder();

    auto dstDesc = getChildEdgeAt(0)->getMemory().GetDescWithType<BlockedMemoryDesc>();
    params.dst_block_dims = dstDesc->getBlockDims();
    params.dst_block_order = dstDesc->getOrder();

    permuteKernel = std::unique_ptr<PermuteKernel>(new PermuteKernel(params));
}

template <typename T>
static void transpose_to_0312(const int MB, const MKLDNNMemoryPtr& srcMemPtr, MKLDNNMemoryPtr& dstMemPtr) {
    const auto src_data = reinterpret_cast<const T*>(srcMemPtr->GetPtr());
    auto dst_data = reinterpret_cast<T*>(dstMemPtr->GetPtr());

    const int DIM1 = srcMemPtr->getStaticDims()[1];
    const int DIM2 = srcMemPtr->getStaticDims()[2];
    const int DIM3 = srcMemPtr->getStaticDims()[3];

    parallel_for3d(MB, DIM1, DIM2, [&](const int n, const int dim1, const int dim2) {
        for (int dim3 = 0; dim3 < DIM3; ++dim3) {
            const int src_off = n * DIM1 * DIM2 * DIM3 +
                                dim1 * DIM2 * DIM3 +
                                dim2 * DIM3 +
                                dim3;
            const int dst_off = n * DIM1 * DIM2 * DIM3 +
                                dim3 * DIM1 * DIM2 +
                                dim1 * DIM2 +
                                dim2;

            dst_data[dst_off] = src_data[src_off];
        }
    });
}

template<typename T>
static void transpose_to_04123(const int MB, const MKLDNNMemoryPtr& srcMemPtr, MKLDNNMemoryPtr& dstMemPtr) {
    const auto src_data = reinterpret_cast<const T*>(srcMemPtr->GetPtr());
    auto dst_data = reinterpret_cast<T*>(dstMemPtr->GetPtr());

    const int DIM1 = srcMemPtr->getStaticDims()[1];
    const int DIM2 = srcMemPtr->getStaticDims()[2];
    const int DIM3 = srcMemPtr->getStaticDims()[3];
    const int DIM4 = srcMemPtr->getStaticDims()[4];

    parallel_for4d(MB, DIM1, DIM2, DIM3, [&](const int n, const int dim1, const int dim2, const int dim3) {
        for (int dim4 = 0; dim4 < DIM4; ++dim4) {
            const int src_off = n * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim1 * DIM2 * DIM3 * DIM4 +
                                dim2 * DIM3 * DIM4 +
                                dim3 * DIM4 +
                                dim4;
            const int dst_off = n * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim4 * DIM1 * DIM2 * DIM3 +
                                dim1 * DIM2 * DIM3 +
                                dim2 * DIM3 +
                                dim3;

            dst_data[dst_off] = src_data[src_off];
        }
    });
}

template<typename T>
static void transpose_to_051234(const int MB, const MKLDNNMemoryPtr& srcMemPtr, MKLDNNMemoryPtr& dstMemPtr) {
    const auto src_data = reinterpret_cast<const T*>(srcMemPtr->GetPtr());
    auto dst_data = reinterpret_cast<T*>(dstMemPtr->GetPtr());

    const int DIM1 = srcMemPtr->getStaticDims()[1];
    const int DIM2 = srcMemPtr->getStaticDims()[2];
    const int DIM3 = srcMemPtr->getStaticDims()[3];
    const int DIM4 = srcMemPtr->getStaticDims()[4];
    const int DIM5 = srcMemPtr->getStaticDims()[5];

    parallel_for5d(MB, DIM1, DIM2, DIM3, DIM4, [&](const int n, const int dim1, const int dim2, const int dim3, const int dim4) {
        for (int dim5 = 0; dim5 < DIM5; ++dim5) {
            const int src_off = n * DIM1 * DIM2 * DIM3 * DIM4 * DIM5 +
                                dim1 * DIM2 * DIM3 * DIM4 * DIM5 +
                                dim2 * DIM3 * DIM4 * DIM5 +
                                dim3 * DIM4 * DIM5 +
                                dim4 * DIM5 +
                                dim5;
            const int dst_off = n * DIM5 * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim5 * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim1 * DIM2 * DIM3 * DIM4 +
                                dim2 * DIM3 * DIM4 +
                                dim3 * DIM4 +
                                dim4;

            dst_data[dst_off] = src_data[src_off];
        }
    });
}

template<typename T>
void MKLDNNTransposeNode::optimizedExecute(const int MB, const MKLDNNMemoryPtr& srcMemPtr, MKLDNNMemoryPtr& dstMemPtr) {
    switch (srcMemPtr->getStaticDims().size()) {
        case 4:
            transpose_to_0312<T>(MB, srcMemPtr, dstMemPtr);
            break;
        case 5:
            transpose_to_04123<T>(MB, srcMemPtr, dstMemPtr);
            break;
        case 6:
            transpose_to_051234<T>(MB, srcMemPtr, dstMemPtr);
            break;
        default:
            IE_THROW() << "Transpose '" << getName() << "' supports optimized execution with only 4D, 5D and 6D shapes";
    }
}

void MKLDNNTransposeNode::execute(mkldnn::stream strm) {
    auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto &srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    int MB = batchToProcess();

    if (isOptimized) {
        const size_t dataSize = getParentEdgeAt(0)->getMemory().getDesc().getPrecision().size();
        TransposeContext ctx = {this, srcMemPtr, dstMemPtr, MB};
        OV_SWITCH(MKLDNNPlugin, TransposeOptimizedEmitter, ctx, dataSize,
                  OV_CASE(1, PrecisionTrait<Precision::U8>::value_type),
                  OV_CASE(2, PrecisionTrait<Precision::U16>::value_type),
                  OV_CASE(4, PrecisionTrait<Precision::I32>::value_type));

        return;
    }

    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(srcMemPtr->GetPtr());
    uint8_t* dstData = reinterpret_cast<uint8_t*>(dstMemPtr->GetPtr());
    permuteKernel->execute(srcData, dstData, MB);
}

bool MKLDNNTransposeNode::created() const {
    return getType() == Transpose;
}
REG_MKLDNN_PRIM_FOR(MKLDNNTransposeNode, Transpose);
