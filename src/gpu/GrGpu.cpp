
/*
 * Copyright 2010 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#include "GrGpu.h"

#include "GrBufferAllocPool.h"
#include "GrContext.h"
#include "GrDrawTargetCaps.h"
#include "GrIndexBuffer.h"
#include "GrStencilBuffer.h"
#include "GrVertexBuffer.h"

// probably makes no sense for this to be less than a page
static const size_t VERTEX_POOL_VB_SIZE = 1 << 18;
static const int VERTEX_POOL_VB_COUNT = 4;
static const size_t INDEX_POOL_IB_SIZE = 1 << 16;
static const int INDEX_POOL_IB_COUNT = 4;

////////////////////////////////////////////////////////////////////////////////

#define DEBUG_INVAL_BUFFER    0xdeadcafe
#define DEBUG_INVAL_START_IDX -1

GrGpu::GrGpu(GrContext* context)
    : fResetTimestamp(kExpiredTimestamp+1)
    , fResetBits(kAll_GrBackendState)
    , fVertexPool(NULL)
    , fIndexPool(NULL)
    , fVertexPoolUseCnt(0)
    , fIndexPoolUseCnt(0)
    , fQuadIndexBuffer(NULL)
    , fContext(context) {
    fGeomPoolStateStack.push_back();
    fDrawState = &fDefaultDrawState;
    // We assume that fDrawState always owns a ref to the object it points at.
    fDefaultDrawState.ref();
#ifdef SK_DEBUG
    GeometryPoolState& poolState = fGeomPoolStateStack.back();
    poolState.fPoolVertexBuffer = (GrVertexBuffer*)DEBUG_INVAL_BUFFER;
    poolState.fPoolStartVertex = DEBUG_INVAL_START_IDX;
    poolState.fPoolIndexBuffer = (GrIndexBuffer*)DEBUG_INVAL_BUFFER;
    poolState.fPoolStartIndex = DEBUG_INVAL_START_IDX;
#endif

    GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.push_back();
#ifdef SK_DEBUG
    geoSrc.fVertexCount = DEBUG_INVAL_START_IDX;
    geoSrc.fVertexBuffer = (GrVertexBuffer*)DEBUG_INVAL_BUFFER;
    geoSrc.fIndexCount = DEBUG_INVAL_START_IDX;
    geoSrc.fIndexBuffer = (GrIndexBuffer*)DEBUG_INVAL_BUFFER;
#endif
    geoSrc.fVertexSrc = GrDrawTarget::kNone_GeometrySrcType;
    geoSrc.fIndexSrc  = GrDrawTarget::kNone_GeometrySrcType;
}

GrGpu::~GrGpu() {
    SkSafeSetNull(fQuadIndexBuffer);
    delete fVertexPool;
    fVertexPool = NULL;
    delete fIndexPool;
    fIndexPool = NULL;
    SkASSERT(1 == fGeoSrcStateStack.count());
    SkDEBUGCODE(GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.back());
    SkASSERT(GrDrawTarget::kNone_GeometrySrcType == geoSrc.fIndexSrc);
    SkASSERT(GrDrawTarget::kNone_GeometrySrcType == geoSrc.fVertexSrc);
    SkSafeUnref(fDrawState);
}

void GrGpu::contextAbandoned() {}

////////////////////////////////////////////////////////////////////////////////

GrTexture* GrGpu::createTexture(const GrSurfaceDesc& desc,
                                const void* srcData, size_t rowBytes) {
    if (!this->caps()->isConfigTexturable(desc.fConfig)) {
        return NULL;
    }

    if ((desc.fFlags & kRenderTarget_GrSurfaceFlag) &&
        !this->caps()->isConfigRenderable(desc.fConfig, desc.fSampleCnt > 0)) {
        return NULL;
    }

    GrTexture *tex = NULL;
    if (GrPixelConfigIsCompressed(desc.fConfig)) {
        // We shouldn't be rendering into this
        SkASSERT((desc.fFlags & kRenderTarget_GrSurfaceFlag) == 0);

        if (!this->caps()->npotTextureTileSupport() &&
            (!SkIsPow2(desc.fWidth) || !SkIsPow2(desc.fHeight))) {
            return NULL;
        }

        this->handleDirtyContext();
        tex = this->onCreateCompressedTexture(desc, srcData);
    } else {
        this->handleDirtyContext();
        tex = this->onCreateTexture(desc, srcData, rowBytes);
        if (tex &&
            (kRenderTarget_GrSurfaceFlag & desc.fFlags) &&
            !(kNoStencil_GrSurfaceFlag & desc.fFlags)) {
            SkASSERT(tex->asRenderTarget());
            // TODO: defer this and attach dynamically
            if (!this->attachStencilBufferToRenderTarget(tex->asRenderTarget())) {
                tex->unref();
                return NULL;
            }
        }
    }
    return tex;
}

bool GrGpu::attachStencilBufferToRenderTarget(GrRenderTarget* rt) {
    SkASSERT(NULL == rt->getStencilBuffer());
    GrStencilBuffer* sb =
        this->getContext()->findStencilBuffer(rt->width(),
                                              rt->height(),
                                              rt->numSamples());
    if (sb) {
        rt->setStencilBuffer(sb);
        bool attached = this->attachStencilBufferToRenderTarget(sb, rt);
        if (!attached) {
            rt->setStencilBuffer(NULL);
        }
        return attached;
    }
    if (this->createStencilBufferForRenderTarget(rt,
                                                 rt->width(), rt->height())) {
        // Right now we're clearing the stencil buffer here after it is
        // attached to an RT for the first time. When we start matching
        // stencil buffers with smaller color targets this will no longer
        // be correct because it won't be guaranteed to clear the entire
        // sb.
        // We used to clear down in the GL subclass using a special purpose
        // FBO. But iOS doesn't allow a stencil-only FBO. It reports unsupported
        // FBO status.
        this->clearStencil(rt);
        return true;
    } else {
        return false;
    }
}

GrTexture* GrGpu::wrapBackendTexture(const GrBackendTextureDesc& desc) {
    this->handleDirtyContext();
    GrTexture* tex = this->onWrapBackendTexture(desc);
    if (NULL == tex) {
        return NULL;
    }
    // TODO: defer this and attach dynamically
    GrRenderTarget* tgt = tex->asRenderTarget();
    if (tgt &&
        !this->attachStencilBufferToRenderTarget(tgt)) {
        tex->unref();
        return NULL;
    } else {
        return tex;
    }
}

GrRenderTarget* GrGpu::wrapBackendRenderTarget(const GrBackendRenderTargetDesc& desc) {
    this->handleDirtyContext();
    return this->onWrapBackendRenderTarget(desc);
}

GrVertexBuffer* GrGpu::createVertexBuffer(size_t size, bool dynamic) {
    this->handleDirtyContext();
    return this->onCreateVertexBuffer(size, dynamic);
}

GrIndexBuffer* GrGpu::createIndexBuffer(size_t size, bool dynamic) {
    this->handleDirtyContext();
    return this->onCreateIndexBuffer(size, dynamic);
}

GrIndexBuffer* GrGpu::createInstancedIndexBuffer(const uint16_t* pattern,
                                                 int patternSize,
                                                 int reps,
                                                 int vertCount,
                                                 bool isDynamic) {
    size_t bufferSize = patternSize * reps * sizeof(uint16_t);
    GrGpu* me = const_cast<GrGpu*>(this);
    GrIndexBuffer* buffer = me->createIndexBuffer(bufferSize, isDynamic);
    if (buffer) {
        uint16_t* data = (uint16_t*) buffer->map();
        bool useTempData = (NULL == data);
        if (useTempData) {
            data = SkNEW_ARRAY(uint16_t, reps * patternSize);
        }
        for (int i = 0; i < reps; ++i) {
            int baseIdx = i * patternSize;
            uint16_t baseVert = (uint16_t)(i * vertCount);
            for (int j = 0; j < patternSize; ++j) {
                data[baseIdx+j] = baseVert + pattern[j];
            }
        }
        if (useTempData) {
            if (!buffer->updateData(data, bufferSize)) {
                SkFAIL("Can't get indices into buffer!");
            }
            SkDELETE_ARRAY(data);
        } else {
            buffer->unmap();
        }
    }
    return buffer;
}

void GrGpu::clear(const SkIRect* rect,
                  GrColor color,
                  bool canIgnoreRect,
                  GrRenderTarget* renderTarget) {
    SkASSERT(renderTarget);
    this->handleDirtyContext();
    this->onGpuClear(renderTarget, rect, color, canIgnoreRect);
}

void GrGpu::clearStencilClip(const SkIRect& rect,
                             bool insideClip,
                             GrRenderTarget* renderTarget) {
    if (NULL == renderTarget) {
        renderTarget = this->getDrawState().getRenderTarget();
    }
    if (NULL == renderTarget) {
        SkASSERT(0);
        return;
    }
    this->handleDirtyContext();
    this->onClearStencilClip(renderTarget, rect, insideClip);
}

bool GrGpu::readPixels(GrRenderTarget* target,
                       int left, int top, int width, int height,
                       GrPixelConfig config, void* buffer,
                       size_t rowBytes) {
    this->handleDirtyContext();
    return this->onReadPixels(target, left, top, width, height,
                              config, buffer, rowBytes);
}

bool GrGpu::writeTexturePixels(GrTexture* texture,
                               int left, int top, int width, int height,
                               GrPixelConfig config, const void* buffer,
                               size_t rowBytes) {
    this->handleDirtyContext();
    return this->onWriteTexturePixels(texture, left, top, width, height,
                                      config, buffer, rowBytes);
}

void GrGpu::resolveRenderTarget(GrRenderTarget* target) {
    SkASSERT(target);
    this->handleDirtyContext();
    this->onResolveRenderTarget(target);
}

void GrGpu::initCopySurfaceDstDesc(const GrSurface* src, GrSurfaceDesc* desc) {
    // Make the dst of the copy be a render target because the default copySurface draws to the dst.
    desc->fOrigin = kDefault_GrSurfaceOrigin;
    desc->fFlags = kRenderTarget_GrSurfaceFlag | kNoStencil_GrSurfaceFlag;
    desc->fConfig = src->config();
}

typedef GrTraceMarkerSet::Iter TMIter;
void GrGpu::saveActiveTraceMarkers() {
    if (this->caps()->gpuTracingSupport()) {
        SkASSERT(0 == fStoredTraceMarkers.count());
        fStoredTraceMarkers.addSet(fActiveTraceMarkers);
        for (TMIter iter = fStoredTraceMarkers.begin(); iter != fStoredTraceMarkers.end(); ++iter) {
            this->removeGpuTraceMarker(&(*iter));
        }
    }
}

void GrGpu::restoreActiveTraceMarkers() {
    if (this->caps()->gpuTracingSupport()) {
        SkASSERT(0 == fActiveTraceMarkers.count());
        for (TMIter iter = fStoredTraceMarkers.begin(); iter != fStoredTraceMarkers.end(); ++iter) {
            this->addGpuTraceMarker(&(*iter));
        }
        for (TMIter iter = fActiveTraceMarkers.begin(); iter != fActiveTraceMarkers.end(); ++iter) {
            this->fStoredTraceMarkers.remove(*iter);
        }
    }
}

void GrGpu::addGpuTraceMarker(const GrGpuTraceMarker* marker) {
    if (this->caps()->gpuTracingSupport()) {
        SkASSERT(fGpuTraceMarkerCount >= 0);
        this->fActiveTraceMarkers.add(*marker);
        this->didAddGpuTraceMarker();
        ++fGpuTraceMarkerCount;
    }
}

void GrGpu::removeGpuTraceMarker(const GrGpuTraceMarker* marker) {
    if (this->caps()->gpuTracingSupport()) {
        SkASSERT(fGpuTraceMarkerCount >= 1);
        this->fActiveTraceMarkers.remove(*marker);
        this->didRemoveGpuTraceMarker();
        --fGpuTraceMarkerCount;
    }
}

void GrGpu::setVertexSourceToBuffer(const GrVertexBuffer* buffer) {
    this->releasePreviousVertexSource();
    GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.back();
    geoSrc.fVertexSrc    = GrDrawTarget::kBuffer_GeometrySrcType;
    geoSrc.fVertexBuffer = buffer;
    buffer->ref();
    geoSrc.fVertexSize = this->drawState()->getVertexStride();
}

void GrGpu::setIndexSourceToBuffer(const GrIndexBuffer* buffer) {
    this->releasePreviousIndexSource();
    GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.back();
    geoSrc.fIndexSrc     = GrDrawTarget::kBuffer_GeometrySrcType;
    geoSrc.fIndexBuffer  = buffer;
    buffer->ref();
}

void GrGpu::setDrawState(GrDrawState*  drawState) {
    SkASSERT(fDrawState);
    if (NULL == drawState) {
        drawState = &fDefaultDrawState;
    }
    if (fDrawState != drawState) {
        fDrawState->unref();
        drawState->ref();
        fDrawState = drawState;
    }
}

void GrGpu::resetVertexSource() {
    this->releasePreviousVertexSource();
    GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.back();
    geoSrc.fVertexSrc = GrDrawTarget::kNone_GeometrySrcType;
}

void GrGpu::resetIndexSource() {
    this->releasePreviousIndexSource();
    GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.back();
    geoSrc.fIndexSrc = GrDrawTarget::kNone_GeometrySrcType;
}

void GrGpu::pushGeometrySource() {
    this->geometrySourceWillPush();
    GrDrawTarget::GeometrySrcState& newState = fGeoSrcStateStack.push_back();
    newState.fIndexSrc = GrDrawTarget::kNone_GeometrySrcType;
    newState.fVertexSrc = GrDrawTarget::kNone_GeometrySrcType;
#ifdef SK_DEBUG
    newState.fVertexCount  = ~0;
    newState.fVertexBuffer = (GrVertexBuffer*)~0;
    newState.fIndexCount   = ~0;
    newState.fIndexBuffer = (GrIndexBuffer*)~0;
#endif
}

void GrGpu::popGeometrySource() {
    // if popping last element then pops are unbalanced with pushes
    SkASSERT(fGeoSrcStateStack.count() > 1);

    this->geometrySourceWillPop(fGeoSrcStateStack.fromBack(1));
    this->releasePreviousVertexSource();
    this->releasePreviousIndexSource();
    fGeoSrcStateStack.pop_back();
}

////////////////////////////////////////////////////////////////////////////////

static const int MAX_QUADS = 1 << 12; // max possible: (1 << 14) - 1;

GR_STATIC_ASSERT(4 * MAX_QUADS <= 65535);

static const uint16_t gQuadIndexPattern[] = {
  0, 1, 2, 0, 2, 3
};

const GrIndexBuffer* GrGpu::getQuadIndexBuffer() const {
    if (NULL == fQuadIndexBuffer || fQuadIndexBuffer->wasDestroyed()) {
        SkSafeUnref(fQuadIndexBuffer);
        GrGpu* me = const_cast<GrGpu*>(this);
        fQuadIndexBuffer = me->createInstancedIndexBuffer(gQuadIndexPattern,
                                                          6,
                                                          MAX_QUADS,
                                                          4);
    }

    return fQuadIndexBuffer;
}

////////////////////////////////////////////////////////////////////////////////

void GrGpu::geometrySourceWillPush() {
    const GrDrawTarget::GeometrySrcState& geoSrc = this->getGeomSrc();
    if (GrDrawTarget::kReserved_GeometrySrcType == geoSrc.fVertexSrc) {
        this->finalizeReservedVertices();
    }
    if (GrDrawTarget::kReserved_GeometrySrcType == geoSrc.fIndexSrc) {
        this->finalizeReservedIndices();
    }
    GeometryPoolState& newState = fGeomPoolStateStack.push_back();
#ifdef SK_DEBUG
    newState.fPoolVertexBuffer = (GrVertexBuffer*)DEBUG_INVAL_BUFFER;
    newState.fPoolStartVertex = DEBUG_INVAL_START_IDX;
    newState.fPoolIndexBuffer = (GrIndexBuffer*)DEBUG_INVAL_BUFFER;
    newState.fPoolStartIndex = DEBUG_INVAL_START_IDX;
#else
    (void) newState; // silence compiler warning
#endif
}

void GrGpu::geometrySourceWillPop(const GrDrawTarget::GeometrySrcState& restoredState) {
    // if popping last entry then pops are unbalanced with pushes
    SkASSERT(fGeomPoolStateStack.count() > 1);
    fGeomPoolStateStack.pop_back();
}

void GrGpu::onDraw(const GrDrawTarget::DrawInfo& info,
                   const GrClipMaskManager::ScissorState& scissorState) {
    this->handleDirtyContext();
    if (!this->flushGraphicsState(PrimTypeToDrawType(info.primitiveType()),
                                  scissorState,
                                  info.getDstCopy())) {
        return;
    }
    this->onGpuDraw(info);
}

void GrGpu::onStencilPath(const GrPath* path,
                          const GrClipMaskManager::ScissorState& scissorState,
                          const GrStencilSettings& stencilSettings) {
    this->handleDirtyContext();

    if (!this->flushGraphicsState(kStencilPath_DrawType, scissorState, NULL)) {
        return;
    }

    this->pathRendering()->stencilPath(path, stencilSettings);
}


void GrGpu::onDrawPath(const GrPath* path,
                       const GrClipMaskManager::ScissorState& scissorState,
                       const GrStencilSettings& stencilSettings,
                       const GrDeviceCoordTexture* dstCopy) {
    this->handleDirtyContext();

    if (!this->flushGraphicsState(kDrawPath_DrawType, scissorState, dstCopy)) {
        return;
    }

    this->pathRendering()->drawPath(path, stencilSettings);
}

void GrGpu::onDrawPaths(const GrPathRange* pathRange,
                        const uint32_t indices[],
                        int count,
                        const float transforms[],
                        GrDrawTarget::PathTransformType transformsType,
                        const GrClipMaskManager::ScissorState& scissorState,
                        const GrStencilSettings& stencilSettings,
                        const GrDeviceCoordTexture* dstCopy) {
    this->handleDirtyContext();

    if (!this->flushGraphicsState(kDrawPaths_DrawType, scissorState, dstCopy)) {
        return;
    }

    pathRange->willDrawPaths(indices, count);
    this->pathRendering()->drawPaths(pathRange, indices, count, transforms, transformsType,
                                     stencilSettings);
}

void GrGpu::finalizeReservedVertices() {
    SkASSERT(fVertexPool);
    fVertexPool->unmap();
}

void GrGpu::finalizeReservedIndices() {
    SkASSERT(fIndexPool);
    fIndexPool->unmap();
}

void GrGpu::prepareVertexPool() {
    if (NULL == fVertexPool) {
        SkASSERT(0 == fVertexPoolUseCnt);
        fVertexPool = SkNEW_ARGS(GrVertexBufferAllocPool, (this, true,
                                                  VERTEX_POOL_VB_SIZE,
                                                  VERTEX_POOL_VB_COUNT));
        fVertexPool->releaseGpuRef();
    } else if (!fVertexPoolUseCnt) {
        // the client doesn't have valid data in the pool
        fVertexPool->reset();
    }
}

void GrGpu::prepareIndexPool() {
    if (NULL == fIndexPool) {
        SkASSERT(0 == fIndexPoolUseCnt);
        fIndexPool = SkNEW_ARGS(GrIndexBufferAllocPool, (this, true,
                                                INDEX_POOL_IB_SIZE,
                                                INDEX_POOL_IB_COUNT));
        fIndexPool->releaseGpuRef();
    } else if (!fIndexPoolUseCnt) {
        // the client doesn't have valid data in the pool
        fIndexPool->reset();
    }
}

bool GrGpu::onReserveVertexSpace(size_t vertexSize,
                                 int vertexCount,
                                 void** vertices) {
    GeometryPoolState& geomPoolState = fGeomPoolStateStack.back();

    SkASSERT(vertexCount > 0);
    SkASSERT(vertices);

    this->prepareVertexPool();

    *vertices = fVertexPool->makeSpace(vertexSize,
                                       vertexCount,
                                       &geomPoolState.fPoolVertexBuffer,
                                       &geomPoolState.fPoolStartVertex);
    if (NULL == *vertices) {
        return false;
    }
    ++fVertexPoolUseCnt;
    return true;
}

bool GrGpu::onReserveIndexSpace(int indexCount, void** indices) {
    GeometryPoolState& geomPoolState = fGeomPoolStateStack.back();

    SkASSERT(indexCount > 0);
    SkASSERT(indices);

    this->prepareIndexPool();

    *indices = fIndexPool->makeSpace(indexCount,
                                     &geomPoolState.fPoolIndexBuffer,
                                     &geomPoolState.fPoolStartIndex);
    if (NULL == *indices) {
        return false;
    }
    ++fIndexPoolUseCnt;
    return true;
}

void GrGpu::releaseReservedVertexSpace() {
    const GrDrawTarget::GeometrySrcState& geoSrc = this->getGeomSrc();
    SkASSERT(GrDrawTarget::kReserved_GeometrySrcType == geoSrc.fVertexSrc);
    size_t bytes = geoSrc.fVertexCount * geoSrc.fVertexSize;
    fVertexPool->putBack(bytes);
    --fVertexPoolUseCnt;
}

void GrGpu::releaseReservedIndexSpace() {
    const GrDrawTarget::GeometrySrcState& geoSrc = this->getGeomSrc();
    SkASSERT(GrDrawTarget::kReserved_GeometrySrcType == geoSrc.fIndexSrc);
    size_t bytes = geoSrc.fIndexCount * sizeof(uint16_t);
    fIndexPool->putBack(bytes);
    --fIndexPoolUseCnt;
}

void GrGpu::releasePreviousVertexSource() {
    GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.back();
    switch (geoSrc.fVertexSrc) {
        case GrDrawTarget::kNone_GeometrySrcType:
            break;
        case GrDrawTarget::kReserved_GeometrySrcType:
            this->releaseReservedVertexSpace();
            break;
        case GrDrawTarget::kBuffer_GeometrySrcType:
            geoSrc.fVertexBuffer->unref();
#ifdef SK_DEBUG
            geoSrc.fVertexBuffer = (GrVertexBuffer*)DEBUG_INVAL_BUFFER;
#endif
            break;
        default:
            SkFAIL("Unknown Vertex Source Type.");
            break;
    }
}

void GrGpu::releasePreviousIndexSource() {
    GrDrawTarget::GeometrySrcState& geoSrc = fGeoSrcStateStack.back();
    switch (geoSrc.fIndexSrc) {
        case GrDrawTarget::kNone_GeometrySrcType:   // these two don't require
            break;
        case GrDrawTarget::kReserved_GeometrySrcType:
            this->releaseReservedIndexSpace();
            break;
        case GrDrawTarget::kBuffer_GeometrySrcType:
            geoSrc.fIndexBuffer->unref();
#ifdef SK_DEBUG
            geoSrc.fIndexBuffer = (GrIndexBuffer*)DEBUG_INVAL_BUFFER;
#endif
            break;
        default:
            SkFAIL("Unknown Index Source Type.");
            break;
    }
}

void GrGpu::releaseGeometry() {
    int popCnt = fGeoSrcStateStack.count() - 1;
    while (popCnt) {
        this->popGeometrySource();
        --popCnt;
    }
    this->resetVertexSource();
    this->resetIndexSource();
}
