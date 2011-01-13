/*
 * Copyright (C) 2010 The Android Open Source Project
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

#define LOG_TAG "SurfaceTexture"
//#define LOG_NDEBUG 0

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <gui/SurfaceTexture.h>

#include <surfaceflinger/ISurfaceComposer.h>
#include <surfaceflinger/SurfaceComposerClient.h>
#include <surfaceflinger/IGraphicBufferAlloc.h>

#include <utils/Log.h>

namespace android {

// Transform matrices
static float mtxIdentity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};
static float mtxFlipH[16] = {
    -1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    1, 0, 0, 1,
};
static float mtxFlipV[16] = {
    1, 0, 0, 0,
    0, -1, 0, 0,
    0, 0, 1, 0,
    0, 1, 0, 1,
};
static float mtxRot90[16] = {
    0, 1, 0, 0,
    -1, 0, 0, 0,
    0, 0, 1, 0,
    1, 0, 0, 1,
};
static float mtxRot180[16] = {
    -1, 0, 0, 0,
    0, -1, 0, 0,
    0, 0, 1, 0,
    1, 1, 0, 1,
};
static float mtxRot270[16] = {
    0, -1, 0, 0,
    1, 0, 0, 0,
    0, 0, 1, 0,
    0, 1, 0, 1,
};

static void mtxMul(float out[16], const float a[16], const float b[16]);

SurfaceTexture::SurfaceTexture(GLuint tex) :
    mBufferCount(MIN_BUFFER_SLOTS), mCurrentTexture(INVALID_BUFFER_SLOT),
    mLastQueued(INVALID_BUFFER_SLOT), mTexName(tex) {
    LOGV("SurfaceTexture::SurfaceTexture");
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        mSlots[i].mEglImage = EGL_NO_IMAGE_KHR;
        mSlots[i].mEglDisplay = EGL_NO_DISPLAY;
        mSlots[i].mOwnedByClient = false;
    }
    sp<ISurfaceComposer> composer(ComposerService::getComposerService());
    mGraphicBufferAlloc = composer->createGraphicBufferAlloc();
}

SurfaceTexture::~SurfaceTexture() {
    LOGV("SurfaceTexture::~SurfaceTexture");
    freeAllBuffers();
}

status_t SurfaceTexture::setBufferCount(int bufferCount) {
    LOGV("SurfaceTexture::setBufferCount");
    Mutex::Autolock lock(mMutex);
    freeAllBuffers();
    mBufferCount = bufferCount;
    mCurrentTexture = INVALID_BUFFER_SLOT;
    mLastQueued = INVALID_BUFFER_SLOT;
    return OK;
}

sp<GraphicBuffer> SurfaceTexture::requestBuffer(int buf,
        uint32_t w, uint32_t h, uint32_t format, uint32_t usage) {
    LOGV("SurfaceTexture::requestBuffer");
    Mutex::Autolock lock(mMutex);
    if (buf < 0 || mBufferCount <= buf) {
        LOGE("requestBuffer: slot index out of range [0, %d]: %d",
                mBufferCount, buf);
        return 0;
    }
    usage |= GraphicBuffer::USAGE_HW_TEXTURE;
    sp<GraphicBuffer> graphicBuffer(
            mGraphicBufferAlloc->createGraphicBuffer(w, h, format, usage));
    if (graphicBuffer == 0) {
        LOGE("requestBuffer: SurfaceComposer::createGraphicBuffer failed");
    } else {
        mSlots[buf].mGraphicBuffer = graphicBuffer;
        if (mSlots[buf].mEglImage != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(mSlots[buf].mEglDisplay, mSlots[buf].mEglImage);
            mSlots[buf].mEglImage = EGL_NO_IMAGE_KHR;
            mSlots[buf].mEglDisplay = EGL_NO_DISPLAY;
        }
        mAllocdBuffers.add(graphicBuffer);
    }
    return graphicBuffer;
}

status_t SurfaceTexture::dequeueBuffer(int *buf) {
    LOGV("SurfaceTexture::dequeueBuffer");
    Mutex::Autolock lock(mMutex);
    int found = INVALID_BUFFER_SLOT;
    for (int i = 0; i < mBufferCount; i++) {
        if (!mSlots[i].mOwnedByClient && i != mCurrentTexture && i != mLastQueued) {
            mSlots[i].mOwnedByClient = true;
            found = i;
            break;
        }
    }
    if (found == INVALID_BUFFER_SLOT) {
        return -EBUSY;
    }
    *buf = found;
    return OK;
}

status_t SurfaceTexture::queueBuffer(int buf) {
    LOGV("SurfaceTexture::queueBuffer");
    Mutex::Autolock lock(mMutex);
    if (buf < 0 || mBufferCount <= buf) {
        LOGE("queueBuffer: slot index out of range [0, %d]: %d",
                mBufferCount, buf);
        return -EINVAL;
    } else if (!mSlots[buf].mOwnedByClient) {
        LOGE("queueBuffer: slot %d is not owned by the client", buf);
        return -EINVAL;
    } else if (mSlots[buf].mGraphicBuffer == 0) {
        LOGE("queueBuffer: slot %d was enqueued without requesting a buffer",
                buf);
        return -EINVAL;
    }
    mSlots[buf].mOwnedByClient = false;
    mLastQueued = buf;
    mLastQueuedCrop = mNextCrop;
    mLastQueuedTransform = mNextTransform;
    return OK;
}

void SurfaceTexture::cancelBuffer(int buf) {
    LOGV("SurfaceTexture::cancelBuffer");
    Mutex::Autolock lock(mMutex);
    if (buf < 0 || mBufferCount <= buf) {
        LOGE("cancelBuffer: slot index out of range [0, %d]: %d", mBufferCount,
                buf);
        return;
    } else if (!mSlots[buf].mOwnedByClient) {
        LOGE("cancelBuffer: slot %d is not owned by the client", buf);
        return;
    }
    mSlots[buf].mOwnedByClient = false;
}

status_t SurfaceTexture::setCrop(const Rect& crop) {
    LOGV("SurfaceTexture::setCrop");
    Mutex::Autolock lock(mMutex);
    mNextCrop = crop;
    return OK;
}

status_t SurfaceTexture::setTransform(uint32_t transform) {
    LOGV("SurfaceTexture::setTransform");
    Mutex::Autolock lock(mMutex);
    mNextTransform = transform;
    return OK;
}

status_t SurfaceTexture::updateTexImage() {
    LOGV("SurfaceTexture::updateTexImage");
    Mutex::Autolock lock(mMutex);

    // We always bind the texture even if we don't update its contents.
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mTexName);

    // Initially both mCurrentTexture and mLastQueued are INVALID_BUFFER_SLOT,
    // so this check will fail until a buffer gets queued.
    if (mCurrentTexture != mLastQueued) {
        // Update the GL texture object.
        EGLImageKHR image = mSlots[mLastQueued].mEglImage;
        if (image == EGL_NO_IMAGE_KHR) {
            EGLDisplay dpy = eglGetCurrentDisplay();
            sp<GraphicBuffer> graphicBuffer = mSlots[mLastQueued].mGraphicBuffer;
            image = createImage(dpy, graphicBuffer);
            mSlots[mLastQueued].mEglImage = image;
            mSlots[mLastQueued].mEglDisplay = dpy;
        }
        glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);
        GLint error = glGetError();
        if (error != GL_NO_ERROR) {
            LOGE("error binding external texture image %p (slot %d): %#04x",
                    image, mLastQueued, error);
            return -EINVAL;
        }

        // Update the SurfaceTexture state.
        mCurrentTexture = mLastQueued;
        mCurrentTextureBuf = mSlots[mCurrentTexture].mGraphicBuffer;
        mCurrentCrop = mLastQueuedCrop;
        mCurrentTransform = mLastQueuedTransform;
    }
    return OK;
}

void SurfaceTexture::getTransformMatrix(float mtx[16]) {
    LOGV("SurfaceTexture::updateTexImage");
    Mutex::Autolock lock(mMutex);

    float* xform = mtxIdentity;
    switch (mCurrentTransform) {
        case 0:
            xform = mtxIdentity;
            break;
        case NATIVE_WINDOW_TRANSFORM_FLIP_H:
            xform = mtxFlipH;
            break;
        case NATIVE_WINDOW_TRANSFORM_FLIP_V:
            xform = mtxFlipV;
            break;
        case NATIVE_WINDOW_TRANSFORM_ROT_90:
            xform = mtxRot90;
            break;
        case NATIVE_WINDOW_TRANSFORM_ROT_180:
            xform = mtxRot180;
            break;
        case NATIVE_WINDOW_TRANSFORM_ROT_270:
            xform = mtxRot270;
            break;
        default:
            LOGE("getTransformMatrix: unknown transform: %d", mCurrentTransform);
    }

    sp<GraphicBuffer>& buf(mSlots[mCurrentTexture].mGraphicBuffer);
    float tx = float(mCurrentCrop.left) / float(buf->getWidth());
    float ty = float(mCurrentCrop.bottom) / float(buf->getHeight());
    float sx = float(mCurrentCrop.width()) / float(buf->getWidth());
    float sy = float(mCurrentCrop.height()) / float(buf->getHeight());
    float crop[16] = {
        sx, 0, 0, sx*tx,
        0, sy, 0, sy*ty,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    mtxMul(mtx, crop, xform);
}

void SurfaceTexture::freeAllBuffers() {
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        mSlots[i].mGraphicBuffer = 0;
        mSlots[i].mOwnedByClient = false;
        if (mSlots[i].mEglImage != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(mSlots[i].mEglDisplay, mSlots[i].mEglImage);
            mSlots[i].mEglImage = EGL_NO_IMAGE_KHR;
            mSlots[i].mEglDisplay = EGL_NO_DISPLAY;
        }
    }

    int exceptBuf = -1;
    for (size_t i = 0; i < mAllocdBuffers.size(); i++) {
        if (mAllocdBuffers[i] == mCurrentTextureBuf) {
            exceptBuf = i;
            break;
        }
    }
    mAllocdBuffers.clear();
    if (exceptBuf >= 0) {
        mAllocdBuffers.add(mCurrentTextureBuf);
    }
    mGraphicBufferAlloc->freeAllGraphicBuffersExcept(exceptBuf);
}

EGLImageKHR SurfaceTexture::createImage(EGLDisplay dpy,
        const sp<GraphicBuffer>& graphicBuffer) {
    EGLClientBuffer cbuf = (EGLClientBuffer)graphicBuffer->getNativeBuffer();
    EGLint attrs[] = {
        EGL_IMAGE_PRESERVED_KHR,    EGL_TRUE,
        EGL_NONE,
    };
    EGLImageKHR image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID, cbuf, attrs);
    EGLint error = eglGetError();
    if (error != EGL_SUCCESS) {
        LOGE("error creating EGLImage: %#x", error);
    } else if (image == EGL_NO_IMAGE_KHR) {
        LOGE("no error reported, but no image was returned by "
                "eglCreateImageKHR");
    }
    return image;
}

static void mtxMul(float out[16], const float a[16], const float b[16]) {
    out[0] = a[0]*b[0] + a[4]*b[1] + a[8]*b[2] + a[12]*b[3];
    out[1] = a[1]*b[0] + a[5]*b[1] + a[9]*b[2] + a[13]*b[3];
    out[2] = a[2]*b[0] + a[6]*b[1] + a[10]*b[2] + a[14]*b[3];
    out[3] = a[3]*b[0] + a[7]*b[1] + a[11]*b[2] + a[15]*b[3];

    out[4] = a[0]*b[4] + a[4]*b[5] + a[8]*b[6] + a[12]*b[7];
    out[5] = a[1]*b[4] + a[5]*b[5] + a[9]*b[6] + a[13]*b[7];
    out[6] = a[2]*b[4] + a[6]*b[5] + a[10]*b[6] + a[14]*b[7];
    out[7] = a[3]*b[4] + a[7]*b[5] + a[11]*b[6] + a[15]*b[7];

    out[8] = a[0]*b[8] + a[4]*b[9] + a[8]*b[10] + a[12]*b[11];
    out[9] = a[1]*b[8] + a[5]*b[9] + a[9]*b[10] + a[13]*b[11];
    out[10] = a[2]*b[8] + a[6]*b[9] + a[10]*b[10] + a[14]*b[11];
    out[11] = a[3]*b[8] + a[7]*b[9] + a[11]*b[10] + a[15]*b[11];

    out[12] = a[0]*b[12] + a[4]*b[13] + a[8]*b[14] + a[12]*b[15];
    out[13] = a[1]*b[12] + a[5]*b[13] + a[9]*b[14] + a[13]*b[15];
    out[14] = a[2]*b[12] + a[6]*b[13] + a[10]*b[14] + a[14]*b[15];
    out[15] = a[3]*b[12] + a[7]*b[13] + a[11]*b[14] + a[15]*b[15];
}

}; // namespace android
