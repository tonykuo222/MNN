//
//  Expr.cpp
//  MNN
//
//  Created by MNN on 2019/06/10.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#define FLATBUFFERS_PREFER_PRINTF
#include <MNN/expr/Expr.hpp>
#include <MNN/expr/ExprCreator.hpp>
#include <map>
#include "core/MNNMemoryUtils.h"
#include "Utils.hpp"
#include <map>
#include "core/FileLoader.hpp"
#include <MNN/expr/Executor.hpp>
#include "flatbuffers/util.h"
#include "MNN_generated.h"
#define MNN_OPEN_TIME_TRACE
#include "MNN/AutoTime.hpp"

//#define MNN_EXPRESS_ERROR_REPORT
static inline std::string numberToString(int index) {
    return flatbuffers::NumToString(index);
}

namespace MNN {
namespace Express {
void Variable::Info::syncSize() {
    size = 1;
    for (int i=0; i<dim.size(); ++i) {
        if (order == NC4HW4 && i == 1) {
            size *= (UP_DIV(dim[1], 4) * 4);
        } else {
            size *= dim[i];
        }
    }
}

bool VARP::fix(VARP::InputType type) const {
    if (nullptr == mContent->expr().first->get()) {
        mContent->expr().first->mType = type;
        return true;
    }
    auto info = mContent->getInfo();
    if (nullptr == info) {
        return false;
    }
    VARP newVar;
    switch (type) {
        case INPUT: {
            newVar = _Input(info->dim, info->order, info->type);
            auto ptr = mContent->readMap<void>();
            if (nullptr != ptr) {
                auto dstPtr = newVar->writeMap<void>();
                ::memcpy(dstPtr, ptr, info->size * info->type.bytes());
            }
            break;
        }
        case CONST: {
            auto ptr = mContent->readMap<void>();
            if (nullptr == ptr) {
                return false;
            }
            newVar = _Const(ptr, info->dim, info->order, info->type);
            break;
        }
        case TRAINABLE: {
            auto ptr = mContent->readMap<void>();
            if (nullptr == ptr) {
                return false;
            }
            newVar = _TrainableParam(ptr, info->dim, info->order, info->type);
            break;
        }
        default:
            return false;
    }
    auto temp = VARP(mContent);
    Variable::replace(temp, newVar);
    return true;
}

struct Expr::Inside {
    std::vector<const Variable::Info*> mInputInfos;
    std::vector<Variable::Info> mOutputInfos;
    Executor::Requirement mReq;
};
Expr::Expr(int outputSize) {
    mInside.reset(new Inside);
    mInside->mOutputInfos.resize(outputSize);
    mOutputNames.resize(outputSize);
}

Expr::~Expr() {
    if (nullptr != mExecutor) {
        mExecutor->recycle(this);
    }
    mInside.reset();
}
void Expr::set(const OpT* op) {
    MNN_ASSERT(nullptr != op);
    flatbuffers::FlatBufferBuilder builder;
    auto offset = Op::Pack(builder, op);
    builder.Finish(offset);
    mExtraBuffer.reset(new char[builder.GetSize()]);
    ::memcpy(mExtraBuffer.get(), builder.GetBufferPointer(), builder.GetSize());
    mOp = flatbuffers::GetMutableRoot<Op>(mExtraBuffer.get());
    mOpBufferSize = builder.GetSize();
    mContentDirty = true;
    mInfoDirty = true;
}
Variable::Info* Expr::outputInfo(int index) {
    return mInside->mOutputInfos.data() + index;
}

void Expr::_addLinkForInputs(EXPRP expr) {
    auto inputs = expr->inputs();
    auto exe = expr->mExecutor;
    if (exe == nullptr) {
        for (auto v : inputs) {
            exe = v->expr().first->mExecutor;
            if (nullptr != exe) {
                break;
            }
        }
    }
    if (exe == nullptr && (inputs.size() > 0 || expr->mType == VARP::INPUT)) {
        exe = Executor::getGlobalExecutor();
    }
    expr->mExecutor = exe;
    for (int i=0; i<inputs.size(); ++i) {
        bool findEmpty = false;
        auto inputExpr = inputs[i]->mFrom;
        for (int j=0; j<inputExpr->mTo.size(); ++j) {
            auto ref = inputExpr->mTo[j].lock();
            if (nullptr == ref) {
                inputExpr->mTo[j] = WeakEXPRP(expr);
                findEmpty = true;
                break;
            }
        }
        if (!findEmpty) {
            inputExpr->mTo.emplace_back(WeakEXPRP(expr));
        }
    }
}
EXPRP Expr::create(Variable::Info&& info, std::shared_ptr<Executor> executor) {
    EXPRP expr(new Expr(1));
    expr->mExecutor = executor;
    expr->mOp = nullptr;
    auto originPtr = info.ptr;
    expr->mInside->mOutputInfos[0] = std::move(info);
    auto& dstInfo = expr->mInside->mOutputInfos[0];
    dstInfo.syncSize();
    if (dstInfo.size > 0) {
        expr->mExtraBuffer.reset(new char[dstInfo.size * dstInfo.type.bytes()]);
        expr->mInside->mOutputInfos[0].ptr = expr->mExtraBuffer.get();
        expr->mInfoDirty = false;
    } else {
        expr->mInside->mOutputInfos[0].ptr = nullptr;
        expr->mInfoDirty = true;
    }
    if (nullptr == originPtr) {
        expr->mType = VARP::INPUT;
        expr->mContentDirty = true;
        return expr;
    }
    expr->mType = VARP::CONST;
    expr->mContentDirty = false;
    ::memcpy(expr->mInside->mOutputInfos[0].ptr, originPtr, dstInfo.size * dstInfo.type.bytes());
    return expr;
}

EXPRP Expr::create(const OpT* op, std::vector<VARP> inputs, int outputSize, std::shared_ptr<Executor> exe) {
    EXPRP expr(new Expr(outputSize));
    if (OpType_Input == op->type) {
        Variable::Info info;
        info.dim = op->main.AsInput()->dims;
        if (info.dim.size() >= 1 && -1 == info.dim[0]) {
            info.dim[0] = 1;
        }
        info.order = Utils::revertFormat(op->main.AsInput()->dformat);
        info.ptr = nullptr;
        info.type = Utils::revertDataType(op->main.AsInput()->dtype);
        return create(std::move(info), exe);
    }
    if (OpType_Const == op->type || OpType_TrainableParam == op->type) {
        Variable::Info info;
        info.dim = op->main.AsBlob()->dims;
        info.order = Utils::revertFormat(op->main.AsBlob()->dataFormat);
        info.ptr = nullptr;
        info.type = Utils::revertDataType(op->main.AsBlob()->dataType);
        switch (op->main.AsBlob()->dataType) {
            case DataType_DT_INT8:
                info.ptr = (void*)op->main.AsBlob()->int8s.data();
                break;
            case DataType_DT_INT32:
                info.ptr = (void*)op->main.AsBlob()->int32s.data();
                break;
            case DataType_DT_UINT8:
                info.ptr = (void*)op->main.AsBlob()->uint8s.data();
                break;
            case DataType_DT_FLOAT:
                info.ptr = (void*)op->main.AsBlob()->float32s.data();
                break;
            default:
                break;
        }
        auto expr = create(std::move(info), exe);
        if (OpType_TrainableParam == op->type) {
            expr->mType = VARP::TRAINABLE;
        }
        return expr;
    }
    expr->mExecutor = exe;
    expr->set(op);
    expr->mInputs   = std::move(inputs);
    _addLinkForInputs(expr);
    return expr;
}
void Expr::setName(const std::string& name) {
    mName = name;
}
bool Expr::requireInfo() {
    if (nullptr == mOp) {
        return true;
    }
    if (!mInfoDirty) {
        return true;
    }
    if (!mValid) {
        return false;
    }
    bool ready     = true;
    mInside->mInputInfos.resize(mInputs.size());
    if (mInside->mReq.shapeNeedContent.empty()) {
        mInside->mReq = mExecutor->onGetRequirement(this);
    }
    for (int i = 0; i < mInputs.size(); ++i) {
        if (nullptr == mInputs[i] || nullptr == mInputs[i]->mFrom) {
            // The Variable is set nullptr by api
            return false;
        }
        mInside->mInputInfos[i] = mInputs[i]->getInfo();
        if (nullptr == mInside->mInputInfos[i] && (!mInside->mReq.supportError[i])) {
#ifdef MNN_EXPRESS_ERROR_REPORT
            MNN_ERROR("%s, %d input not ready\n", mName.c_str(), i);
#endif
            mValid = false;
            return false;
        }
    }
    for (int i = 0; i < mInputs.size(); ++i) {
        auto& v  = mInputs[i];
        if (mInside->mReq.shapeNeedContent[i]) {
            auto res = v->expr().first->requireCompute();
            if (!res) {
#ifdef MNN_EXPRESS_ERROR_REPORT
                MNN_ERROR("%s, Error for compute shape %d\n", mName.c_str(), i);
#endif
                ready = false;
                mValid = false;
                break;
            }
        }
    }
    if (!ready) {
        return false;
    }
    //MNN_PRINT("Info %s, %p Start\n", mName.c_str(), this);
    auto res   = mExecutor->onComputeInfo(this);
    //MNN_PRINT("Info Compute %s\n", mName.c_str());

    if (NO_ERROR == res) {
        mInfoDirty = false;
    } else {
        mValid = false;
    }
    return NO_ERROR == res;
}

bool Expr::requireCompute() {
    if (nullptr == mOp) {
        if (mType == VARP::INPUT) {
            return !mContentDirty;
        }
        return true;
    }
    if ((!mContentDirty) && mValid) {
        return true;
    }
    if (!mValid) {
        return false;
    }
    bool res = requireInfo();
    if (!res) {
        return false;
    }
    for (int i = 0; i < mInputs.size(); ++i) {
        if (mInside->mReq.contentNeedContent[i]) {
            auto& input = mInputs[i];
            auto expr   = input->expr().first;
            res    = expr->requireCompute();
            if (!res) {
#ifdef MNN_EXPRESS_ERROR_REPORT
                MNN_ERROR("%s compute input %d error , \n", mName.c_str(), i);
#endif
                if (!mInside->mReq.supportError[i]) {
                    mValid = false;
                    return false;
                }
            }
        }
    }
    auto code = mExecutor->onComputeContent(this);
    //MNN_PRINT("Compute %s, %p End\n", mName.c_str(), this);
    res = code == NO_ERROR;
    if (!res) {
#ifdef MNN_EXPRESS_ERROR_REPORT
        MNN_ERROR("Error for compute %s\n", mName.c_str());
#endif
        mValid = false;
        return false;
    }
    mContentDirty = false;
    return true;
}

size_t Variable::linkNumber() const {
    return mFrom->outputs().size();
}
const std::vector<WeakEXPRP>& Variable::toExprs() const {
    return mFrom->outputs();
}

VARP Variable::create(EXPRP expr, int index) {
    VARP res(new Variable(expr, index));
    return res;
}
void Expr::replace(EXPRP old, EXPRP from) {
    if (old.get() == from.get()) {
        return;
    }
    MNN_ASSERT(old->outputSize() == from->outputSize());
    for (auto input : from->inputs()) {
        bool findEmpty = false;
        for (int j=0; j<input->mFrom->mTo.size(); ++j) {
            auto ref = input->mFrom->mTo[j].lock();
            if (nullptr == ref) {
                input->mFrom->mTo[j] = WeakEXPRP(old);
                findEmpty = true;
                break;
            }
        }
        if (!findEmpty) {
            input->mFrom->mTo.emplace_back(WeakEXPRP(old));
        }
    }
    if (nullptr != old->mExecutor) {
        old->mExecutor->recycle(old.get());
    }
    old->mOp = from->mOp;
    old->mExtraBuffer = from->mExtraBuffer;
    old->mOpBufferSize = from->mOpBufferSize;
    old->mType = from->mType;
    old->mExecutor = from->mExecutor;
    if (nullptr != old->mExecutor) {
        old->mExecutor->setTheSame(from.get(), old.get());
    }
    old->mInside = from->mInside;
    old->mContentDirty = from->mContentDirty;
    old->mInfoDirty = from->mInfoDirty;
    old->mInputs = from->mInputs;
    old->visitOutputs([&](EXPRP expr, int index) {
        if (expr->mInfoDirty) {
            return false;
        }
        expr->mContentDirty = true;
        expr->mInfoDirty    = true;
        return true;
    });
}

void Variable::setName(const std::string& name) {
    mFrom->mOutputNames[mFromIndex] = name;
    if (mFrom->name().empty()) {
        mFrom->setName(name);
    }
}
const std::string& Variable::name() const {
    return mFrom->outputName(mFromIndex);
}
bool Variable::input(VARP src) {
    if (nullptr != mFrom->get() && VARP::INPUT != mFrom->mType) {
        MNN_ERROR("Can't input to no-input op\n");
        return false;
    }
    if (nullptr == src) {
        /*Close the Input*/
        mFrom->visitOutputs([](EXPRP expr, int index) {
            auto recurse = expr->mValid; expr->mValid = false;
            return recurse;
        });
        mFrom->mValid = false;
        return false;
    }
    auto info = src->getInfo();
    std::shared_ptr<Variable::Info> tempInfo;
    bool needCopy = true;
    if (nullptr == info || 0 == info->size) {
        tempInfo.reset(new Variable::Info);
        tempInfo->type = halide_type_of<float>();
        info = tempInfo.get();
        needCopy = false;
    }
    auto dstInfo = getInfo();
    bool needChange = nullptr == dstInfo || info->order != dstInfo->order || info->dim.size() != dstInfo->dim.size();
    if (!needChange) {
        for (int i=0; i<info->dim.size(); ++i) {
            if (dstInfo->dim[i] != info->dim[i]) {
                needChange = true;
                break;
            }
        }
    }
    if (needChange) {
        bool needAlloc = info->size * info->type.bytes() > mFrom->mInside->mOutputInfos[0].size * mFrom->mInside->mOutputInfos[0].type.bytes();
        mFrom->mInside->mOutputInfos[0] = *info;
        if (needAlloc) {
            mFrom->mExtraBuffer.reset(new char[info->size * info->type.bytes()]);
        }
        mFrom->mInside->mOutputInfos[0].ptr = mFrom->mExtraBuffer.get();
    }
    if (needCopy) {
        auto dstPtr = writeInternal(false);
        auto srcPtr = src->readMap<void>();
        if (nullptr == dstPtr || nullptr == srcPtr) {
            MNN_ERROR("Alloc memory error or compute src error in Variable::Input\n");
            return false;
        }
        ::memcpy(dstPtr, srcPtr, info->size * info->type.bytes());
    }
    if (needChange) {
        mFrom->visitOutputs([](EXPRP expr, int index) { return expr->setInfoDirty(); });
    } else {
        informDirty();
    }
    return true;
}

void Variable::replace(VARP dst, VARP src) {
    if (nullptr == src) {
        dst->setExpr(nullptr, 0);
        return;
    }
    Expr::replace(dst->mFrom, src->mFrom);
    dst->mFromIndex = src->mFromIndex;
}

const Variable::Info* Variable::getInfo() {
    if (nullptr == mFrom) {
        return nullptr;
    }
    auto res = mFrom->requireInfo();
    if (!res) {
        return nullptr;
    }
    return mFrom->mInside->mOutputInfos.data() + mFromIndex;
}

bool Variable::resize(INTS dims) {
    if (nullptr != mFrom->get() && VARP::INPUT != mFrom->mType) {
        MNN_ERROR("Can't resize variable not from input\n");
        return false;
    }
    auto& info = mFrom->mInside->mOutputInfos[0];
    if (dims.size() == info.dim.size()) {
        bool theSame = true;
        for (int i=0; i<dims.size(); ++i) {
            if (info.dim[i] != dims[i]) {
                theSame = false;
                break;
            }
        }
        if (theSame) {
            return true;
        }
    }
    info.dim = dims;
    info.size = 1;
    for (int i=0; i<info.dim.size(); ++i) {
        info.size *= info.dim[i];
    }
    mFrom->mExtraBuffer.reset(new char[info.size * info.type.bytes()]);
    info.ptr = mFrom->mExtraBuffer.get();
    
    mFrom->mContentDirty = true;
    mFrom->mValid = true;
    mFrom->mInside->mInputInfos.clear();

    mFrom->visitOutputs([](EXPRP expr, int index) { return expr->setInfoDirty(); });
    return true;
}
void Expr::visit(EXPRP expr, const std::function<bool(EXPRP)>& before, const std::function<bool(EXPRP)>& after) {
    bool next = before(expr);
    if (!next) {
        return;
    }
    for (int i = 0; i < expr->inputs().size(); ++i) {
        visit(expr->inputs()[i]->mFrom, before, after);
    }
    after(expr);
}

void* Variable::readInternal() {
    if (nullptr == mFrom->get()) {
        if (mFrom->mContentDirty) {
            return nullptr;
        }
        return mFrom->outputInfo(mFromIndex)->ptr;
    }
    auto res = mFrom->requireCompute();
    if (!res) {
        return nullptr;
    }
    return mFrom->outputInfo(mFromIndex)->ptr;
}

void Variable::informDirty() {
    mFrom->visitOutputs([](EXPRP expr, int index) {
        auto needRecurse = expr->setContentDirty(index);
        return needRecurse;
    });
}

void* Variable::writeInternal(bool inform) {
    if (inform) {
        informDirty();
    }
    mFrom->mContentDirty = false;
    return mFrom->mInside->mOutputInfos[0].ptr;
}

void Variable::unMap() {
    //mFrom->inside()->onUnMapContent(mFromIndex);
}

void Expr::visitOutputs(const std::function<bool(EXPRP, int)>& visit) {
    for (auto iter = mTo.begin(); iter != mTo.end();) {
        auto expr = iter->lock();
        if (nullptr == expr) {
            iter = mTo.erase(iter);
            continue;
        }
        bool recurse = false;
        auto inputs = expr->inputs();
        for (int i=0; i<inputs.size(); ++i) {
            if (inputs[i]->mFrom.get() == this) {
                recurse = recurse || visit(expr, i);
            }
        }
        if (recurse) {
            expr->visitOutputs(visit);
        }
        iter++;
    }
}
void Expr::setExecutor(std::shared_ptr<Executor> exe) {
    mExecutor          = exe;
}
bool Expr::setContentDirty(int inputIndex) {
    if (mContentDirty) {
        return false;
    }
    if (nullptr != mInside) {
        if (mInside->mReq.shapeNeedContent[inputIndex]) {
            visitOutputs([](EXPRP expr, int index) { return expr->setInfoDirty(); });
            return setInfoDirty();
        }
        if (!mInside->mReq.contentNeedContent[inputIndex]) {
            return false;
        }
    }
    mContentDirty = true;
    return true;
}
bool Expr::setInfoDirty() {
    if (mInfoDirty && mValid) {
        //MNN_PRINT("End Info Dirty for %s\n", mName.c_str());
        return false;
    }
    //MNN_PRINT("Set Info Dirty for %s\n", mName.c_str());
    mInfoDirty    = true;
    mContentDirty = true;
    mValid = true;
    return true;
}

std::vector<VARP> Variable::load(const char* fileName) {
    AUTOTIME;
    FileLoader loader(fileName);
    if (!loader.valid()) {
        MNN_ERROR("Error for open %s\n", fileName);
        return {};
    }
    loader.read();
    if (!loader.valid()) {
        return {};
    }
    AutoStorage<uint8_t> buffer;
    loader.merge(buffer);
    if (buffer.get() == nullptr) {
        return {};
    }
    flatbuffers::Verifier verify((const uint8_t*)(buffer.get()), buffer.size());
    if (false == VerifyNetBuffer(verify)) {
        MNN_PRINT("Invalidate buffer to create variable\n");
        return {};
    }
    std::unique_ptr<NetT> source(UnPackNet(buffer.get()));
    if (nullptr == source) {
        return {};
    }
    if (source->oplists.empty()) {
        MNN_ERROR("Invalid net\n");
        return {};
    }
    // FUNC_PRINT(source->oplists.size());

    auto opSize      = source->oplists.size();
    auto tensorCount = source->tensorName.size();
    if (tensorCount == 0) {
        tensorCount = source->tensorNumber;
    }
    std::vector<VARP> variable;
    variable.reserve(tensorCount);
    std::map<int, VARP> variableMap;

    // Generate All Exprs by order of net
    for (int i = 0; i < opSize; ++i) {
        std::vector<VARP> inputs;
        auto op = source->oplists[i].get();
        for (int index = 0; index < op->inputIndexes.size(); ++index) {
            auto inputIndex = op->inputIndexes[index];
            if (variableMap.find(inputIndex) == variableMap.end()) {
                MNN_ERROR("Can't find variable for %s, the graph is error\n", op->name.c_str());
                break;
            }
            inputs.emplace_back(variableMap[inputIndex]);
        }
        EXPRP expr = Expr::create(source->oplists[i].get(), inputs, (int)op->outputIndexes.size());
        expr->setName(source->oplists[i]->name);

        for (int index = 0; index < op->outputIndexes.size(); ++index) {
            auto outputIndex = op->outputIndexes[index];
            if (variableMap.find(outputIndex) == variableMap.end()) {
                auto newVariable = Variable::create(expr, index);
                if (source->tensorName.size() > outputIndex) {
                    newVariable->setName(source->tensorName[outputIndex]);
                }
                variableMap[outputIndex] = newVariable;
                variable.emplace_back(newVariable);
            }
        }
    }
    return variable;
}
std::map<std::string, VARP> Variable::loadMap(const char* fileName) {
    AUTOTIME;
    auto variables = load(fileName);
    std::map<std::string, VARP> varMap;
    for (auto v : variables) {
        varMap[v->name()] = v;
    }
    return varMap;
}
std::vector<VARP> Variable::mapToSequence(const std::map<std::string, VARP>& source) {
    std::vector<VARP> outputs;
    outputs.reserve(source.size());
    for (auto& iter : source) {
        outputs.emplace_back(iter.second);
    }
    return outputs;
}
void Variable::save(const std::vector<VARP>& vars, NetT* dest) {
    auto executeOrder = getExecuteOrder(vars);

    // Get Expr - TensorOffset Map
    std::map<EXPRP, int> varIndexInfo;
    {
        int tensorOffset = 0;
        for (int i=0; i<executeOrder.size(); ++i) {
            auto expr = executeOrder[i];
            auto outputSize = executeOrder[i]->outputSize();
            varIndexInfo[expr] = tensorOffset;
            tensorOffset += outputSize;
        }
        dest->tensorName.resize(tensorOffset);
    }

    // Create All Op
    for (int index = 0; index < executeOrder.size(); ++index) {
        auto expr = executeOrder[index];
        auto mOp = expr->get();
        std::unique_ptr<OpT> op;
        if (nullptr != mOp) {
            op.reset(mOp->UnPack());
        } else {
            MNN_ASSERT(1 == expr->outputSize());
            auto& info = expr->mInside->mOutputInfos[0];
            op.reset(new OpT);
            if (expr->mType != VARP::INPUT) {
                auto blob        = new BlobT;
                blob->dataFormat = (MNN_DATA_FORMAT)Utils::convertFormat(info.order);
                blob->dims       = info.dim;
                if (info.type.code == halide_type_float) {
                    blob->dataType = DataType_DT_FLOAT;
                    blob->float32s.resize(info.size);
                    ::memcpy(blob->float32s.data(), info.ptr, info.size * sizeof(float));
                } else if (info.type.code == halide_type_int) {
                    blob->dataType = DataType_DT_INT32;
                    blob->int32s.resize(info.size);
                    ::memcpy(blob->int32s.data(), info.ptr, info.size * sizeof(int));
                }
                else if (info.type.code == halide_type_uint && info.type.bits == 8) {
                    blob->dataType = DataType_DT_UINT8;
                    blob->uint8s.resize(info.size);
                    ::memcpy(blob->uint8s.data(), info.ptr, info.size * sizeof(uint8_t));
                }
                op->type       = OpType_Const;
                if (expr->mType == VARP::TRAINABLE) {
                    op->type = OpType_TrainableParam;
                }
                op->main.type  = OpParameter_Blob;
                op->main.value = blob;
            } else {
                op->type                    = OpType_Input;
                op->main.type               = OpParameter_Input;
                op->main.value              = new InputT;
                op->main.AsInput()->dtype   = (MNN::DataType)Utils::convertDataType(info.type);
                MNN_ASSERT(op->main.AsInput()->dtype != DataType_DT_INVALID);
                op->main.AsInput()->dims    = info.dim;
                op->main.AsInput()->dformat = (MNN_DATA_FORMAT)Utils::convertFormat(info.order);
            }
        }
        op->name = expr->name();
        op->inputIndexes.resize(expr->inputs().size());
        for (int i = 0; i < op->inputIndexes.size(); ++i) {
            auto inputExpr = expr->inputs()[i]->expr();
            op->inputIndexes[i] = varIndexInfo[inputExpr.first] + inputExpr.second;
        }
        if (op->name.empty()) {
            op->name = EnumNameOpType(op->type) + numberToString(index+1);
        }
        op->outputIndexes.resize(expr->outputSize());
        auto tensorIndexOffset = varIndexInfo[expr];
        for (int v=0; v<expr->outputSize(); ++v) {
            op->outputIndexes[v] = tensorIndexOffset + v;
            dest->tensorName[tensorIndexOffset+v] = expr->outputName(v);
        }
        dest->oplists.emplace_back(std::move(op));
    }

    // Fill Empty Tensor Name With Default Op Name
    for (int index = 0; index < executeOrder.size(); ++index) {
        auto expr = executeOrder[index];
        auto op = dest->oplists[index].get();
        auto tensorIndexOffset = varIndexInfo[expr];
        for (int v=0; v<expr->outputSize(); ++v) {
            auto index = tensorIndexOffset + v;
            if (dest->tensorName[index].empty()) {
                if (v == 0) {
                    dest->tensorName[index] = op->name;
                } else {
                    dest->tensorName[index] = op->name + numberToString(v);
                }
            }
        }
    }
}
void Variable::save(const std::vector<VARP>& vars, const char* fileName) {
    std::unique_ptr<NetT> net(new NetT);
    save(vars, net.get());
    // FUNC_PRINT(net->oplists.size());
    flatbuffers::FlatBufferBuilder builder(1024);
    auto offset = Net::Pack(builder, net.get());
    builder.Finish(offset);
    // TODO, use FileWriter instead
    FILE* f = fopen(fileName, "wb");
    if (nullptr == f) {
        MNN_ERROR("Open %s error\n", fileName);
        return;
    }
    static const size_t block = 4096;
    size_t totalSize    = builder.GetSize();
    size_t blockSize    = UP_DIV(totalSize, block);
    for (size_t i = 0; i < blockSize; ++i) {
        size_t sta = block * i;
        size_t fin = std::min(sta + block, totalSize);
        if (fin > sta) {
            auto realSize = fwrite((const char*)builder.GetBufferPointer() + sta, 1, fin - sta, f);
            if (realSize != fin - sta) {
                MNN_ERROR("Write %s error\n", fileName);
            }
        }
    }
    fclose(f);
}
std::pair<std::map<std::string, VARP>, std::map<std::string, VARP>> Variable::getInputAndOutput(const std::map<std::string, VARP>& allVariable) {
    std::pair<std::map<std::string, VARP>, std::map<std::string, VARP>> res;
    for (auto& iter : allVariable) {
        auto var = iter.second;
        if (var->expr().first->get() == nullptr && var->expr().first->mType == VARP::INPUT) {
            res.first[var->name()] = var;
        }
        if (var->linkNumber() == 0) {
            res.second[var->name()] = var;
        }
    }
    return res;
}

std::vector<EXPRP> Variable::getExecuteOrder(const std::vector<VARP>& outputs) {
    std::vector<EXPRP> sequence;
    for (auto output : outputs) {
        Expr::visit(
                        output->mFrom, [](EXPRP expr) { return !expr->visited(); },
                        [&sequence](EXPRP expr) {
                            //FUNC_PRINT_ALL(var->name().c_str(), s);
                            if (!expr->visited()) {
                                sequence.emplace_back(expr);
                                expr->setVisited(true);
                            }
                            return true;
                        });
    }
    for (auto expr : sequence) {
        expr->setVisited(false);
    }
    return sequence;
}

VARP VARP::operator+(VARP var) const {
    return _Add(VARP(mContent), var);
}
VARP VARP::operator-(VARP var) const {
    return _Subtract(VARP(mContent), var);
}
VARP VARP::operator*(VARP var) const {
    return _Multiply(VARP(mContent), var);
}
VARP VARP::operator/(VARP var) const {
    return _Divide(VARP(mContent), var);
}
VARP VARP::mean(INTS dims) const {
    return _ReduceMean(VARP(mContent), dims);
}
VARP VARP::sum(INTS dims) const {
    return _ReduceSum(VARP(mContent), dims);
}

} // namespace Express
} // namespace MNN
