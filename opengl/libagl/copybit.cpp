/*
**
** Copyright 2009, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdlib.h>
#include <stdio.h>

#include "context.h"
#include "fp.h"
#include "state.h"
#include "matrix.h"
#include "vertex.h"
#include "light.h"
#include "primitives.h"
#include "texture.h"
#include "BufferObjectManager.h"
#include "TextureObjectManager.h"

#include <hardware/gralloc.h>
#include <hardware/copybit.h>
#include <private/ui/android_natives_priv.h>


#define DEBUG_COPYBIT true

// ----------------------------------------------------------------------------

namespace android {

static void textureToCopyBitImage(
        const GGLSurface* surface, buffer_handle_t buffer, copybit_image_t* img) 
{
    img->w      = surface->stride;
    img->h      = surface->height;
    img->format = surface->format;
    img->base   = surface->data;
    img->handle = (native_handle_t *)buffer;
}

struct clipRectRegion : public copybit_region_t {
    clipRectRegion(ogles_context_t* c) 
    {
        scissor_t const* scissor = &c->rasterizer.state.scissor;
        r.l = scissor->left;
        r.t = scissor->top;
        r.r = scissor->right;
        r.b = scissor->bottom;
        next = iterate; 
    }
private:
    static int iterate(copybit_region_t const * self, copybit_rect_t* rect) {
        *rect = static_cast<clipRectRegion const*>(self)->r;
        const_cast<copybit_region_t *>(self)->next = iterate_done;
        return 1;
    }
    static int iterate_done(copybit_region_t const *, copybit_rect_t*) {
        return 0;
    }
    copybit_rect_t r;
};

static bool supportedCopybitsFormat(int format) {
    switch (format) {
    case COPYBIT_FORMAT_RGBA_8888:
    case COPYBIT_FORMAT_RGBX_8888:
    case COPYBIT_FORMAT_RGB_888:
    case COPYBIT_FORMAT_RGB_565:
    case COPYBIT_FORMAT_BGRA_8888:
    case COPYBIT_FORMAT_RGBA_5551:
    case COPYBIT_FORMAT_RGBA_4444:
    case COPYBIT_FORMAT_YCbCr_422_SP:
    case COPYBIT_FORMAT_YCbCr_420_SP:
        return true;
    default:
        return false;
    }
}

static bool hasAlpha(int format) {
    switch (format) {
    case COPYBIT_FORMAT_RGBA_8888:
    case COPYBIT_FORMAT_BGRA_8888:
    case COPYBIT_FORMAT_RGBA_5551:
    case COPYBIT_FORMAT_RGBA_4444:
        return true;
    default:
        return false;
    }
}

static inline int fixedToByte(GGLfixed val) {
    return (val - (val >> 8)) >> 8;
}

/**
 * Performs a quick check of the rendering state. If this function returns
 * false we cannot use the copybit driver.
 */

static bool checkContext(ogles_context_t* c) {

	// By convention copybitQuickCheckContext() has already returned true.
	// avoid checking the same information again.
	
    if (c->copybits.blitEngine == NULL) {
        LOGD_IF(DEBUG_COPYBIT, "no copybit hal");
        return false;
    }

    if (c->rasterizer.state.enables
                    & (GGL_ENABLE_DEPTH_TEST|GGL_ENABLE_FOG)) {
        LOGD_IF(DEBUG_COPYBIT, "depth test and/or fog");
        return false;
    }

    // Note: The drawSurfaceBuffer is only set for destination
    // surfaces types that are supported by the hardware and
    // do not have an alpha channel. So we don't have to re-check that here.

    static const int tmu = 0;
    texture_unit_t& u(c->textures.tmu[tmu]);
    EGLTextureObject* textureObject = u.texture;

    if (!supportedCopybitsFormat(textureObject->surface.format)) {
        LOGD_IF(DEBUG_COPYBIT, "texture format not supported");
        return false;
    }
    return true;
}


static bool copybit(GLint x, GLint y,
        GLint w, GLint h,
        EGLTextureObject* textureObject,
        const GLint* crop_rect,
        int transform,
        ogles_context_t* c)
{
    // We assume checkContext has already been called and has already
    // returned true.

    const GGLSurface& cbSurface = c->rasterizer.state.buffers.color.s;

    y = cbSurface.height - (y + h);

    const GLint Ucr = crop_rect[0];
    const GLint Vcr = crop_rect[1];
    const GLint Wcr = crop_rect[2];
    const GLint Hcr = crop_rect[3];

    GLint screen_w = w;
    GLint screen_h = h;
    int32_t dsdx = Wcr << 16;   // dsdx =  ((Wcr/screen_w)/Wt)*Wt
    int32_t dtdy = Hcr << 16;   // dtdy = -((Hcr/screen_h)/Ht)*Ht
    if (transform & COPYBIT_TRANSFORM_ROT_90) {
        swap(screen_w, screen_h);
    }
    if (dsdx!=screen_w || dtdy!=screen_h) {
        // in most cases the divide is not needed
        dsdx /= screen_w;
        dtdy /= screen_h;
    }
    dtdy = -dtdy; // see equation of dtdy above
    if (dsdx < c->copybits.minScale || dsdx > c->copybits.maxScale
            || dtdy < c->copybits.minScale || dtdy > c->copybits.maxScale) {
        // The requested scale is out of the range the hardware
        // can support.
        LOGD_IF(DEBUG_COPYBIT,
                "scale out of range dsdx=%08x (Wcr=%d / w=%d), "
                "dtdy=%08x (Hcr=%d / h=%d), Ucr=%d, Vcr=%d", 
                dsdx, Wcr, w, dtdy, Hcr, h, Ucr, Vcr);
        return false;
    }

    // copybit doesn't say anything about filtering, so we can't
    // discriminate. On msm7k, copybit will always filter.
    // the code below handles min/mag filters, we keep it as a reference.
    
#ifdef MIN_MAG_FILTER
    int32_t texelArea = gglMulx(dtdy, dsdx);
    if (texelArea < FIXED_ONE && textureObject->mag_filter != GL_LINEAR) {
        // Non-linear filtering on a texture enlargement.
        LOGD_IF(DEBUG_COPYBIT, "mag filter is not GL_LINEAR");
        return false;
    }
    if (texelArea > FIXED_ONE && textureObject->min_filter != GL_LINEAR) {
        // Non-linear filtering on an texture shrink.
        LOGD_IF(DEBUG_COPYBIT, "min filter is not GL_LINEAR");
        return false;
    }
#endif
    
    const uint32_t enables = c->rasterizer.state.enables;
    int planeAlpha = 255;
    static const int tmu = 0;
    texture_t& tev(c->rasterizer.state.texture[tmu]);
    bool srcTextureHasAlpha = hasAlpha(textureObject->surface.format);
    if (!srcTextureHasAlpha) {
        planeAlpha = fixedToByte(c->currentColorClamped.a);
    }

    switch (tev.env) {
    case GGL_REPLACE:
        break;
    case GGL_MODULATE:
        if (! (c->currentColorClamped.r == FIXED_ONE &&
               c->currentColorClamped.g == FIXED_ONE &&
               c->currentColorClamped.b == FIXED_ONE)) {
            LOGD_IF(DEBUG_COPYBIT, 
                    "MODULATE and non white color (%08x, %08x, %08x)",
                    c->currentColorClamped.r,
                    c->currentColorClamped.g,
                    c->currentColorClamped.b);
            return false;
        }
        if (srcTextureHasAlpha && c->currentColorClamped.a < FIXED_ONE) {
            LOGD_IF(DEBUG_COPYBIT, 
                    "MODULATE and texture w/alpha and alpha=%08x)",
                    c->currentColorClamped.a);
            return false;
        }
        break;

    default:
        // Incompatible texture environment.
        LOGD_IF(DEBUG_COPYBIT, "incompatible texture environment");
        return false;
    }

    bool blending = false;
    if ((enables & GGL_ENABLE_BLENDING)
            && !(c->rasterizer.state.blend.src == GL_ONE
                    && c->rasterizer.state.blend.dst == GL_ZERO)) {
        // Blending is OK if it is
        // the exact kind of blending that the copybits hardware supports.
        // Note: The hardware only supports
        // GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA,
        // But the surface flinger uses GL_ONE / GL_ONE_MINUS_SRC_ALPHA.
        // We substitute GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA in that case,
        // because the performance is worth it, even if the results are
        // not correct.
        if (!((c->rasterizer.state.blend.src == GL_SRC_ALPHA
                || c->rasterizer.state.blend.src == GL_ONE)
                && c->rasterizer.state.blend.dst == GL_ONE_MINUS_SRC_ALPHA
                && c->rasterizer.state.blend.alpha_separate == 0)) {
            // Incompatible blend mode.
            LOGD_IF(DEBUG_COPYBIT, "incompatible blend mode");
            return false;
        }
        blending = true;
    } else {
        // No blending is OK if we are not using alpha.
        if (srcTextureHasAlpha || planeAlpha != 255) {
            // Incompatible alpha
            LOGD_IF(DEBUG_COPYBIT, "incompatible alpha");
            return false;
        }
    }

    if (srcTextureHasAlpha && planeAlpha != 255) {
        // Can't do two types of alpha at once.
        LOGD_IF(DEBUG_COPYBIT, "src alpha and plane alpha");
        return false;
    }

    // LOGW("calling copybits");

    copybit_device_t* copybit = c->copybits.blitEngine;

    copybit_image_t dst;
    buffer_handle_t target_hnd = c->copybits.drawSurfaceBuffer;
    textureToCopyBitImage(&cbSurface, target_hnd, &dst);
    copybit_rect_t drect = {x, y, x+w, y+h};

    copybit_image_t src;
    buffer_handle_t source_hnd = textureObject->buffer->handle;
    textureToCopyBitImage(&textureObject->surface, source_hnd, &src);
    copybit_rect_t srect = { Ucr, Vcr + Hcr, Ucr + Wcr, Vcr };

    copybit->set_parameter(copybit, COPYBIT_TRANSFORM, transform);
    copybit->set_parameter(copybit, COPYBIT_PLANE_ALPHA, planeAlpha);
    copybit->set_parameter(copybit, COPYBIT_DITHER,
            (enables & GGL_ENABLE_DITHER) ? COPYBIT_ENABLE : COPYBIT_DISABLE);

    clipRectRegion it(c);
    status_t err = copybit->stretch(copybit, &dst, &src, &drect, &srect, &it);
    if (err != NO_ERROR) {
        c->textures.tmu[0].texture->try_copybit = false;
    }
    return err == NO_ERROR ? true : false;
}

/*
 * Try to draw a triangle fan with copybit, return false if we fail.
 */
bool drawTriangleFanWithCopybit_impl(ogles_context_t* c, GLint first, GLsizei count)
{
    if (!checkContext(c)) {
        return false;
    }

    // FIXME: we should handle culling  here
    c->arrays.compileElements(c, c->vc.vBuffer, 0, 4);

    // we detect if we're dealing with a rectangle, by comparing the
    // rectangles {v0,v2} and {v1,v3} which should be identical.
    
    // NOTE: we should check that the rectangle is window aligned, however
    // if we do that, the optimization won't be taken in a lot of cases.
    // Since this code is intended to be used with SurfaceFlinger only,
    // so it's okay...
    
    const vec4_t& v0 = c->vc.vBuffer[0].window;
    const vec4_t& v1 = c->vc.vBuffer[1].window;
    const vec4_t& v2 = c->vc.vBuffer[2].window;
    const vec4_t& v3 = c->vc.vBuffer[3].window;
    int l = min(v0.x, v2.x);
    int b = min(v0.y, v2.y);
    int r = max(v0.x, v2.x);
    int t = max(v0.y, v2.y);
    if ((l != min(v1.x, v3.x)) || (b != min(v1.y, v3.y)) ||
        (r != max(v1.x, v3.x)) || (t != max(v1.y, v3.y))) {
        LOGD_IF(DEBUG_COPYBIT, "geometry not a rectangle");
        return false;
    }

    // fetch and transform texture coordinates
    // NOTE: maybe it would be better to have a "compileElementsAll" method
    // that would ensure all vertex data are fetched and transformed
    const transform_t& tr = c->transforms.texture[0].transform; 
    for (size_t i=0 ; i<4 ; i++) {
        const GLubyte* tp = c->arrays.texture[0].element(i);
        vertex_t* const v = &c->vc.vBuffer[i];
        c->arrays.texture[0].fetch(c, v->texture[0].v, tp);
        // FIXME: we should bail if q!=1
        c->arrays.tex_transform[0](&tr, &v->texture[0], &v->texture[0]);
    }
    
    const vec4_t& t0 = c->vc.vBuffer[0].texture[0];
    const vec4_t& t1 = c->vc.vBuffer[1].texture[0];
    const vec4_t& t2 = c->vc.vBuffer[2].texture[0];
    const vec4_t& t3 = c->vc.vBuffer[3].texture[0];
    int txl = min(t0.x, t2.x);
    int txb = min(t0.y, t2.y);
    int txr = max(t0.x, t2.x);
    int txt = max(t0.y, t2.y);
    if ((txl != min(t1.x, t3.x)) || (txb != min(t1.y, t3.y)) ||
        (txr != max(t1.x, t3.x)) || (txt != max(t1.y, t3.y))) {
        LOGD_IF(DEBUG_COPYBIT, "texcoord not a rectangle");
        return false;
    }
    if ((txl != 0) || (txb != 0) ||
        (txr != FIXED_ONE) || (txt != FIXED_ONE)) {
        // we could probably handle this case, if we wanted to
        LOGD_IF(DEBUG_COPYBIT, "texture is cropped: %08x,%08x,%08x,%08x",
                txl, txb, txr, txt);
        return false;
    }

    // at this point, we know we are dealing with a rectangle, so we 
    // only need to consider 3 vertices for computing the jacobians
    
    const int dx01 = v1.x - v0.x;
    const int dx02 = v2.x - v0.x;
    const int dy01 = v1.y - v0.y;
    const int dy02 = v2.y - v0.y;
    const int ds01 = t1.S - t0.S;
    const int ds02 = t2.S - t0.S;
    const int dt01 = t1.T - t0.T;
    const int dt02 = t2.T - t0.T;
    const int area = dx01*dy02 - dy01*dx02;
    int dsdx, dsdy, dtdx, dtdy;
    if (area >= 0) {
        dsdx = ds01*dy02 - ds02*dy01;
        dtdx = dt01*dy02 - dt02*dy01;
        dsdy = ds02*dx01 - ds01*dx02;
        dtdy = dt02*dx01 - dt01*dx02;
    } else {
        dsdx = ds02*dy01 - ds01*dy02;
        dtdx = dt02*dy01 - dt01*dy02;
        dsdy = ds01*dx02 - ds02*dx01;
        dtdy = dt01*dx02 - dt02*dx01;
    }

    // here we rely on the fact that we know the transform is
    // a rigid-body transform AND that it can only rotate in 90 degrees
    // increments

    int transform = 0;
    if (dsdx == 0) {
        // 90 deg rotation case
        // [ 0    dtdx  ]
        // [ dsdx    0  ]
        transform |= COPYBIT_TRANSFORM_ROT_90;
        // FIXME: not sure if FLIP_H and FLIP_V shouldn't be inverted
        if (dtdx > 0)
            transform |= COPYBIT_TRANSFORM_FLIP_H;
        if (dsdy < 0)
            transform |= COPYBIT_TRANSFORM_FLIP_V;
    } else {
        // [ dsdx    0  ]
        // [ 0     dtdy ]
        if (dsdx < 0)
            transform |= COPYBIT_TRANSFORM_FLIP_H;
        if (dtdy < 0)
            transform |= COPYBIT_TRANSFORM_FLIP_V;
    }

    //LOGD("l=%d, b=%d, w=%d, h=%d, tr=%d", x, y, w, h, transform);
    //LOGD("A=%f\tB=%f\nC=%f\tD=%f",
    //      dsdx/65536.0, dtdx/65536.0, dsdy/65536.0, dtdy/65536.0);

    int x = l >> 4;
    int y = b >> 4;
    int w = (r-l) >> 4;
    int h = (t-b) >> 4;
    texture_unit_t& u(c->textures.tmu[0]);
    EGLTextureObject* textureObject = u.texture;
    GLint tWidth = textureObject->surface.width;
    GLint tHeight = textureObject->surface.height;
    GLint crop_rect[4] = {0, tHeight, tWidth, -tHeight};
    const GGLSurface& cbSurface = c->rasterizer.state.buffers.color.s;
    y = cbSurface.height - (y + h);
    return copybit(x, y, w, h, textureObject, crop_rect, transform, c);
}

/*
 * Try to drawTexiOESWithCopybit, return false if we fail.
 */

bool drawTexiOESWithCopybit_impl(GLint x, GLint y, GLint z,
        GLint w, GLint h, ogles_context_t* c)
{
    // quickly process empty rects
    if ((w|h) <= 0) {
        return true;
    }
    if (!checkContext(c)) {
        return false;
    }
    texture_unit_t& u(c->textures.tmu[0]);
    EGLTextureObject* textureObject = u.texture;
    return copybit(x, y, w, h, textureObject, textureObject->crop_rect, 0, c);
}

} // namespace android
