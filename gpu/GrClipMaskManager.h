/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrClipMaskManager_DEFINED
#define GrClipMaskManager_DEFINED

#include "GrClipMaskCache.h"
#include "GrContext.h"
#include "GrDrawState.h"
#include "GrPathRenderer.h"
#include "GrReducedClip.h"
#include "GrStencil.h"
#include "GrTexture.h"

#include "SkClipStack.h"
#include "SkDeque.h"
#include "SkPath.h"
#include "SkRefCnt.h"
#include "SkTLList.h"
#include "SkTypes.h"

class GrGpu;
class GrPathRenderer;
class GrPathRendererChain;
class GrTexture;
class SkPath;

/**
 * The clip mask creator handles the generation of the clip mask. If anti
 * aliasing is requested it will (in the future) generate a single channel
 * (8bit) mask. If no anti aliasing is requested it will generate a 1-bit
 * mask in the stencil buffer. In the non anti-aliasing case, if the clip
 * mask can be represented as a rectangle then scissoring is used. In all
 * cases scissoring is used to bound the range of the clip mask.
 */
class GrClipMaskManager : public SkNoncopyable {
public:
    GrClipMaskManager()
        : fGpu(NULL)
        , fCurrClipMaskType(kNone_ClipMaskType) {
    }

    /**
     * Creates a clip mask if necessary as a stencil buffer or alpha texture
     * and sets the GrGpu's scissor and stencil state. If the return is false
     * then the draw can be skipped.
     */
    bool setupClipping(const GrClipData* clipDataIn, GrDrawState::AutoRestoreEffects*);

    void releaseResources();

    bool isClipInStencil() const {
        return kStencil_ClipMaskType == fCurrClipMaskType;
    }
    bool isClipInAlpha() const {
        return kAlpha_ClipMaskType == fCurrClipMaskType;
    }

    void invalidateStencilMask() {
        if (kStencil_ClipMaskType == fCurrClipMaskType) {
            fCurrClipMaskType = kNone_ClipMaskType;
        }
    }

    GrContext* getContext() {
        return fAACache.getContext();
    }

    void setGpu(GrGpu* gpu);

    void adjustPathStencilParams(GrStencilSettings* settings);
private:
    /**
     * Informs the helper function adjustStencilParams() about how the stencil
     * buffer clip is being used.
     */
    enum StencilClipMode {
        // Draw to the clip bit of the stencil buffer
        kModifyClip_StencilClipMode,
        // Clip against the existing representation of the clip in the high bit
        // of the stencil buffer.
        kRespectClip_StencilClipMode,
        // Neither writing to nor clipping against the clip bit.
        kIgnoreClip_StencilClipMode,
    };

    GrGpu* fGpu;

    /**
     * We may represent the clip as a mask in the stencil buffer or as an alpha
     * texture. It may be neither because the scissor rect suffices or we
     * haven't yet examined the clip.
     */
    enum ClipMaskType {
        kNone_ClipMaskType,
        kStencil_ClipMaskType,
        kAlpha_ClipMaskType,
    } fCurrClipMaskType;

    GrClipMaskCache fAACache;       // cache for the AA path

    // Draws the clip into the stencil buffer
    bool createStencilClipMask(int32_t elementsGenID,
                               GrReducedClip::InitialState initialState,
                               const GrReducedClip::ElementList& elements,
                               const SkIRect& clipSpaceIBounds,
                               const SkIPoint& clipSpaceToStencilOffset);
    // Creates an alpha mask of the clip. The mask is a rasterization of elements through the
    // rect specified by clipSpaceIBounds.
    GrTexture* createAlphaClipMask(int32_t elementsGenID,
                                   GrReducedClip::InitialState initialState,
                                   const GrReducedClip::ElementList& elements,
                                   const SkIRect& clipSpaceIBounds);
    // Similar to createAlphaClipMask but it rasterizes in SW and uploads to the result texture.
    GrTexture* createSoftwareClipMask(int32_t elementsGenID,
                                      GrReducedClip::InitialState initialState,
                                      const GrReducedClip::ElementList& elements,
                                      const SkIRect& clipSpaceIBounds);

    // Gets a texture to use for the clip mask. If true is returned then a cached mask was found
    // that already contains the rasterization of the clip stack, otherwise an uninitialized texture
    // is returned. 'willUpload' is set when the alpha mask needs to be uploaded from the CPU.
    bool getMaskTexture(int32_t elementsGenID,
                        const SkIRect& clipSpaceIBounds,
                        GrTexture** result,
                        bool willUpload);

    bool useSWOnlyPath(const GrReducedClip::ElementList& elements);

    // Draws a filled clip path into the target alpha mask
    bool drawFilledPath(GrTexture* target, GrPathRenderer* pathRenderer, bool isAA);

    // Draws a clip element into the target alpha mask. The caller should have already setup the
    // desired blend operation.
    bool drawElement(GrTexture* target, const SkClipStack::Element* element);

    // Determines whether it is possible to draw the element to both the stencil buffer and the
    // alpha mask simultaneously. If so and the element is a path a compatible path renderer is
    // also returned.
    bool canStencilAndDrawElement(GrTexture* target, const SkClipStack::Element*, 
                                  GrPathRenderer::AutoClearPath* pr);

    void mergeMask(GrTexture* dstMask,
                   GrTexture* srcMask,
                   SkRegion::Op op,
                   const SkIRect& dstBound,
                   const SkIRect& srcBound);

    void getTemp(int width, int height, GrAutoScratchTexture* temp);

    void setupCache(const SkClipStack& clip,
                    const SkIRect& bounds);

    /**
     * Called prior to return control back the GrGpu in setupClipping. It
     * updates the GrGpu with stencil settings that account stencil-based
     * clipping.
     */
    void setGpuStencil();

    /**
     * Adjusts the stencil settings to account for interaction with stencil
     * clipping.
     */
    void adjustStencilParams(GrStencilSettings* settings,
                             StencilClipMode mode,
                             int stencilBitCnt);

    typedef SkNoncopyable INHERITED;
};

#endif // GrClipMaskManager_DEFINED
