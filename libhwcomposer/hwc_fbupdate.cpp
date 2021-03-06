/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are
 * retained for attribution purposes only.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define DEBUG_FBUPDATE 0
#include <cutils/properties.h>
#include <gralloc_priv.h>
#include <overlay.h>
#include <overlayRotator.h>
#include "hwc_fbupdate.h"
#include "mdp_version.h"
#include "external.h"
#include "virtual.h"

using namespace qdutils;
using namespace overlay;
using overlay::Rotator;
using namespace overlay::utils;

namespace qhwc {

namespace ovutils = overlay::utils;

IFBUpdate* IFBUpdate::getObject(hwc_context_t *ctx, const int& dpy) {
    if(isDisplaySplit(ctx, dpy)) {
        return new FBUpdateSplit(dpy);
    }
    return new FBUpdateNonSplit(dpy);
}

inline void IFBUpdate::reset() {
    mModeOn = false;
    mRot = NULL;
}

//================= Low res====================================
FBUpdateNonSplit::FBUpdateNonSplit(const int& dpy): IFBUpdate(dpy) {}

inline void FBUpdateNonSplit::reset() {
    IFBUpdate::reset();
    mDest = ovutils::OV_INVALID;
}

bool FBUpdateNonSplit::preRotateExtDisplay(hwc_context_t *ctx,
                                            ovutils::Whf &info,
                                            hwc_rect_t& sourceCrop,
                                            ovutils::eMdpFlags& mdpFlags,
                                            int& rotFlags)
{
    int extOrient = getExtOrientation(ctx);
    ovutils::eTransform orient = static_cast<ovutils::eTransform >(extOrient);
    if(mDpy && (extOrient & HWC_TRANSFORM_ROT_90)) {
        mRot = ctx->mRotMgr->getNext();
        if(mRot == NULL) return false;
        //Configure rotator for pre-rotation
        if(configRotator(mRot, info, sourceCrop, mdpFlags, orient, 0) < 0) {
            ALOGE("%s: configRotator Failed!", __FUNCTION__);
            mRot = NULL;
            return false;
        }
        info.format = (mRot)->getDstFormat();
        updateSource(orient, info, sourceCrop);
        rotFlags |= ovutils::ROT_PREROTATED;
    }
    return true;
}

bool FBUpdateNonSplit::prepare(hwc_context_t *ctx, hwc_display_contents_1 *list,
                             int fbZorder) {
    if(!ctx->mMDP.hasOverlay) {
        ALOGD_IF(DEBUG_FBUPDATE, "%s, this hw doesnt support overlays",
                 __FUNCTION__);
        return false;
    }
    mModeOn = configure(ctx, list, fbZorder);
    return mModeOn;
}

// Configure
bool FBUpdateNonSplit::configure(hwc_context_t *ctx, hwc_display_contents_1 *list,
                               int fbZorder) {
    bool ret = false;
    hwc_layer_1_t *layer = &list->hwLayers[list->numHwLayers - 1];
    if (LIKELY(ctx->mOverlay)) {
        int extOnlyLayerIndex = ctx->listStats[mDpy].extOnlyLayerIndex;
        // ext only layer present..
        if(extOnlyLayerIndex != -1) {
            layer = &list->hwLayers[extOnlyLayerIndex];
            layer->compositionType = HWC_OVERLAY;
        }
        overlay::Overlay& ov = *(ctx->mOverlay);
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        ovutils::Whf info(hnd->width, hnd->height,
                          ovutils::getMdpFormat(hnd->format), hnd->size);

        //Request a pipe
        ovutils::eMdpPipeType type = ovutils::OV_MDP_PIPE_ANY;
        if(qdutils::MDPVersion::getInstance().is8x26() && mDpy) {
            //For 8x26 external always use DMA pipe
            type = ovutils::OV_MDP_PIPE_DMA;
        }
        ovutils::eDest dest = ov.nextPipe(type, mDpy, Overlay::MIXER_DEFAULT);
        if(dest == ovutils::OV_INVALID) { //None available
            ALOGE("%s: No pipes available to configure fb for dpy %d",
                __FUNCTION__, mDpy);
            return false;
        }
        mDest = dest;

        if((mDpy && ctx->deviceOrientation) &&
            ctx->listStats[mDpy].isDisplayAnimating) {
            fbZorder = 0;
        }

        ovutils::eMdpFlags mdpFlags = ovutils::OV_MDP_BLEND_FG_PREMULT;
        ovutils::eIsFg isFg = ovutils::IS_FG_OFF;
        ovutils::eZorder zOrder = static_cast<ovutils::eZorder>(fbZorder);

        hwc_rect_t sourceCrop = layer->sourceCrop;
        hwc_rect_t displayFrame = layer->displayFrame;
        int transform = layer->transform;
        int rotFlags = ovutils::ROT_FLAGS_NONE;

        ovutils::eTransform orient =
                    static_cast<ovutils::eTransform>(transform);
        // use ext orientation if any
        int extOrient = getExtOrientation(ctx);

        // Do not use getNonWormholeRegion() function to calculate the
        // sourceCrop during animation on external display and
        // Dont do wormhole calculation when extorientation is set on External
        // Dont do wormhole calculation when extDownscale is enabled on External
        if(ctx->listStats[mDpy].isDisplayAnimating && mDpy) {
            sourceCrop = layer->displayFrame;
            displayFrame = sourceCrop;
        } else if((!mDpy ||
                   (mDpy && !extOrient
                   && !ctx->dpyAttr[mDpy].mDownScaleMode))
                   && (extOnlyLayerIndex == -1)) {
            if(!qdutils::MDPVersion::getInstance().is8x26()) {
                getNonWormholeRegion(list, sourceCrop);
                displayFrame = sourceCrop;
            }
        }
        calcExtDisplayPosition(ctx, hnd, mDpy, sourceCrop, displayFrame,
                                   transform, orient);
        setMdpFlags(layer, mdpFlags, 0, transform);
        // For External use rotator if there is a rotation value set
        ret = preRotateExtDisplay(ctx, info, sourceCrop, mdpFlags, rotFlags);
        if(!ret) {
            ALOGE("%s: preRotate for external Failed!", __FUNCTION__);
            return false;
        }
        //For the mdp, since either we are pre-rotating or MDP does flips
        orient = ovutils::OVERLAY_TRANSFORM_0;
        transform = 0;
        ovutils::PipeArgs parg(mdpFlags, info, zOrder, isFg,
                               static_cast<ovutils::eRotFlags>(rotFlags),
                               ovutils::DEFAULT_PLANE_ALPHA,
                               (ovutils::eBlending)
                               getBlending(layer->blending));
        ret = true;
        if(configMdp(ctx->mOverlay, parg, orient, sourceCrop, displayFrame,
                    NULL, mDest) < 0) {
            ALOGE("%s: configMdp failed for dpy %d", __FUNCTION__, mDpy);
            ret = false;
        }
    }
    return ret;
}

bool FBUpdateNonSplit::draw(hwc_context_t *ctx, private_handle_t *hnd)
{
    if(!mModeOn) {
        return true;
    }
    bool ret = true;
    overlay::Overlay& ov = *(ctx->mOverlay);
    ovutils::eDest dest = mDest;
    int fd = hnd->fd;
    uint32_t offset = hnd->offset;
    if(mRot) {
        if(!mRot->queueBuffer(fd, offset))
            return false;
        fd = mRot->getDstMemId();
        offset = mRot->getDstOffset();
    }
    if (!ov.queueBuffer(fd, offset, dest)) {
        ALOGE("%s: queueBuffer failed for FBUpdate", __FUNCTION__);
        ret = false;
    }
    return ret;
}

//================= High res====================================
FBUpdateSplit::FBUpdateSplit(const int& dpy): IFBUpdate(dpy) {}

inline void FBUpdateSplit::reset() {
    IFBUpdate::reset();
    mDestLeft = ovutils::OV_INVALID;
    mDestRight = ovutils::OV_INVALID;
    mRot = NULL;
}

bool FBUpdateSplit::prepare(hwc_context_t *ctx, hwc_display_contents_1 *list,
                              int fbZorder) {
    if(!ctx->mMDP.hasOverlay) {
        ALOGD_IF(DEBUG_FBUPDATE, "%s, this hw doesnt support overlays",
                 __FUNCTION__);
        return false;
    }
    ALOGD_IF(DEBUG_FBUPDATE, "%s, mModeOn = %d", __FUNCTION__, mModeOn);
    mModeOn = configure(ctx, list, fbZorder);
    return mModeOn;
}

// Configure
bool FBUpdateSplit::configure(hwc_context_t *ctx,
        hwc_display_contents_1 *list, int fbZorder) {
    bool ret = false;
    hwc_layer_1_t *layer = &list->hwLayers[list->numHwLayers - 1];
    if (LIKELY(ctx->mOverlay)) {
        int extOnlyLayerIndex = ctx->listStats[mDpy].extOnlyLayerIndex;
        // ext only layer present..
        if(extOnlyLayerIndex != -1) {
            layer = &list->hwLayers[extOnlyLayerIndex];
            layer->compositionType = HWC_OVERLAY;
        }
        overlay::Overlay& ov = *(ctx->mOverlay);
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        ovutils::Whf info(hnd->width, hnd->height,
                          ovutils::getMdpFormat(hnd->format), hnd->size);

        //Request left pipe
        ovutils::eDest destL = ov.nextPipe(ovutils::OV_MDP_PIPE_ANY, mDpy,
                Overlay::MIXER_LEFT);
        if(destL == ovutils::OV_INVALID) { //None available
            ALOGE("%s: No pipes available to configure fb for dpy %d's left"
                    " mixer", __FUNCTION__, mDpy);
            return false;
        }
        //Request right pipe
        ovutils::eDest destR = ov.nextPipe(ovutils::OV_MDP_PIPE_ANY, mDpy,
                Overlay::MIXER_RIGHT);
        if(destR == ovutils::OV_INVALID) { //None available
            ALOGE("%s: No pipes available to configure fb for dpy %d's right"
                    " mixer", __FUNCTION__, mDpy);
            return false;
        }

        mDestLeft = destL;
        mDestRight = destR;

        ovutils::eMdpFlags mdpFlagsL = ovutils::OV_MDP_BLEND_FG_PREMULT;

        ovutils::eZorder zOrder = static_cast<ovutils::eZorder>(fbZorder);

        //XXX: FB layer plane alpha is currently sent as zero from
        //surfaceflinger
        ovutils::PipeArgs pargL(mdpFlagsL,
                                info,
                                zOrder,
                                ovutils::IS_FG_OFF,
                                ovutils::ROT_FLAGS_NONE,
                                ovutils::DEFAULT_PLANE_ALPHA,
                                (ovutils::eBlending)
                                getBlending(layer->blending));
        ov.setSource(pargL, destL);

        ovutils::eMdpFlags mdpFlagsR = mdpFlagsL;
        ovutils::setMdpFlags(mdpFlagsR, ovutils::OV_MDSS_MDP_RIGHT_MIXER);
        ovutils::PipeArgs pargR(mdpFlagsR,
                                info,
                                zOrder,
                                ovutils::IS_FG_OFF,
                                ovutils::ROT_FLAGS_NONE,
                                ovutils::DEFAULT_PLANE_ALPHA,
                                (ovutils::eBlending)
                                getBlending(layer->blending));
        ov.setSource(pargR, destR);

        hwc_rect_t sourceCrop = layer->sourceCrop;
        hwc_rect_t displayFrame = layer->displayFrame;

        const float xres = ctx->dpyAttr[mDpy].xres;
        const int lSplit = getLeftSplit(ctx, mDpy);
        const float lSplitRatio = lSplit / xres;
        const float lCropWidth =
                (sourceCrop.right - sourceCrop.left) * lSplitRatio;

        ovutils::Dim dcropL(
                sourceCrop.left,
                sourceCrop.top,
                lCropWidth,
                sourceCrop.bottom - sourceCrop.top);

        ovutils::Dim dcropR(
                sourceCrop.left + lCropWidth,
                sourceCrop.top,
                (sourceCrop.right - sourceCrop.left) - lCropWidth,
                sourceCrop.bottom - sourceCrop.top);

        ov.setCrop(dcropL, destL);
        ov.setCrop(dcropR, destR);

        int transform = layer->transform;
        ovutils::eTransform orient =
            static_cast<ovutils::eTransform>(transform);
        ov.setTransform(orient, destL);
        ov.setTransform(orient, destR);

        const int lWidth = (lSplit - displayFrame.left);
        const int rWidth = (displayFrame.right - lSplit);
        const int height = displayFrame.bottom - displayFrame.top;

        ovutils::Dim dposL(displayFrame.left,
                           displayFrame.top,
                           lWidth,
                           height);
        ov.setPosition(dposL, destL);

        ovutils::Dim dposR(0,
                           displayFrame.top,
                           rWidth,
                           height);
        ov.setPosition(dposR, destR);

        ret = true;
        if (!ov.commit(destL)) {
            ALOGE("%s: commit fails for left", __FUNCTION__);
            ret = false;
        }
        if (!ov.commit(destR)) {
            ALOGE("%s: commit fails for right", __FUNCTION__);
            ret = false;
        }
    }
    return ret;
}

bool FBUpdateSplit::draw(hwc_context_t *ctx, private_handle_t *hnd)
{
    if(!mModeOn) {
        return true;
    }
    bool ret = true;
    overlay::Overlay& ov = *(ctx->mOverlay);
    ovutils::eDest destL = mDestLeft;
    ovutils::eDest destR = mDestRight;
    if (!ov.queueBuffer(hnd->fd, hnd->offset, destL)) {
        ALOGE("%s: queue failed for left of dpy = %d",
              __FUNCTION__, mDpy);
        ret = false;
    }
    if (!ov.queueBuffer(hnd->fd, hnd->offset, destR)) {
        ALOGE("%s: queue failed for right of dpy = %d",
              __FUNCTION__, mDpy);
        ret = false;
    }
    return ret;
}

//---------------------------------------------------------------------
}; //namespace qhwc
