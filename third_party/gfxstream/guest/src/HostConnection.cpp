/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "HostConnection.h"

#include "aemu/base/threads/AndroidThread.h"
#if defined(__ANDROID__)
#include "android-base/properties.h"
#endif
#include "gfxstream/common/logging.h"
#include "renderControl_types.h"

using gfxstream::guest::ChecksumCalculator;
using gfxstream::guest::IOStream;

#ifdef GOLDFISH_NO_GL
struct gl_client_context_t {
    int placeholder;
};
class GLEncoder : public gl_client_context_t {
public:
    GLEncoder(IOStream*, ChecksumCalculator*) { }
    void setContextAccessor(gl_client_context_t *()) { }
};
struct gl2_client_context_t {
    int placeholder;
};
class GL2Encoder : public gl2_client_context_t {
public:
    GL2Encoder(IOStream*, ChecksumCalculator*) { }
    void setContextAccessor(gl2_client_context_t *()) { }
    void setNoHostError(bool) { }
    void setDrawCallFlushInterval(uint32_t) { }
    void setHasAsyncUnmapBuffer(int) { }
    void setHasSyncBufferData(int) { }
};
#else
#include "GLEncoder.h"
#include "GL2Encoder.h"
#endif

#include <unistd.h>

#include "ProcessPipe.h"
#include "StudAsgStream.h"
#include "StudStream.h"
#include "ThreadInfo.h"
#include "VirtGpu.h"

#if defined(__linux__) || defined(__ANDROID__)
#include <fstream>
#include <string>
#include <unistd.h>
#endif

#define STREAM_BUFFER_SIZE  (4*1024*1024)

static HostConnectionType getConnectionTypeFromProperty(enum VirtGpuCapset) {
    return HOST_CONNECTION_STUD;
}

static uint32_t getDrawCallFlushIntervalFromProperty() {
    constexpr uint32_t kDefaultValue = 800;
    uint32_t value = kDefaultValue;

#if defined(__ANDROID__)
    value = android::base::GetUintProperty("ro.boot.qemu.gltransport.drawFlushInterval",
                                           kDefaultValue);
#endif
    return value;
}

HostConnection::HostConnection() : m_checksumHelper(), m_noHostError(true), m_rendernodeFd(-1) {}

HostConnection::~HostConnection()
{
    // round-trip to ensure that queued commands have been processed
    // before process pipe closure is detected.
    if (m_rcEnc) {
        (void)m_rcEnc->rcGetRendererVersion(m_rcEnc.get());
    }

    if (m_stream) {
        m_stream->decRef();
    }
}

// static
std::unique_ptr<HostConnection> HostConnection::connect(enum VirtGpuCapset capset) {
    const enum HostConnectionType connType = getConnectionTypeFromProperty(capset);
    uint32_t noRenderControlEnc = 0;

    // Use "new" to access a non-public constructor.
    auto con = std::unique_ptr<HostConnection>(new HostConnection);
    con->m_connectionType = connType;

    // gfxstream's ring transport where render-host offers it, else the socket.
    if (gfxstream::guest::IOStream* ring = createStudAsgStream()) {
        con->m_stream = ring;
    } else {
        auto stream = new StudStream(STREAM_BUFFER_SIZE);
        if (stream->connect() < 0) {
            GFXSTREAM_ERROR("Failed to connect to host (StudStream).");
            stream->decRef();
            return nullptr;
        }
        con->m_stream = stream;
    }

#if defined(ANDROID)
    con->m_grallocHelper.reset(gfxstream::createPlatformGralloc(con->m_rendernodeFd));
    if (!con->m_grallocHelper) {
        GFXSTREAM_FATAL("Failed to create platform Gralloc!");
        abort();
    }

    con->m_anwHelper.reset(gfxstream::createPlatformANativeWindowHelper());
    if (!con->m_anwHelper) {
        GFXSTREAM_FATAL("Failed to create platform ANativeWindowHelper!");
        abort();
    }
#endif

    con->m_syncHelper.reset(gfxstream::createPlatformSyncHelper());

    // send zero 'clientFlags' to the host.
    unsigned int *pClientFlags =
            (unsigned int *)con->m_stream->allocBuffer(sizeof(unsigned int));
    *pClientFlags = 0;
    con->m_stream->commitBuffer(sizeof(unsigned int));


    auto handle = (connType == HOST_CONNECTION_VIRTIO_GPU_ADDRESS_SPACE) ? con->m_rendernodeFd : -1;
    processPipeInit(handle, connType, noRenderControlEnc);
    if (!noRenderControlEnc && capset == kCapsetGfxStreamVulkan) {
        con->rcEncoder();
    }

    return con;
}

HostConnection* HostConnection::get() { return getWithThreadInfo(getEGLThreadInfo(), kCapsetNone); }

HostConnection* HostConnection::getOrCreate(enum VirtGpuCapset capset) {
    return getWithThreadInfo(getEGLThreadInfo(), capset);
}

HostConnection* HostConnection::getWithThreadInfo(EGLThreadInfo* tinfo, enum VirtGpuCapset capset) {
    // Get thread info
    if (!tinfo) {
        return NULL;
    }

    if (tinfo->hostConn == NULL) {
        tinfo->hostConn = HostConnection::createUnique(capset);
    }

    return tinfo->hostConn.get();
}

void HostConnection::exit() {
    EGLThreadInfo *tinfo = getEGLThreadInfo();
    if (!tinfo) {
        return;
    }

#if defined(ANDROID)
    if (tinfo->hostConn) {
        tinfo->hostConn->m_grallocHelper = nullptr;
    }
#endif

    tinfo->hostConn.reset();
}

// static
std::unique_ptr<HostConnection> HostConnection::createUnique(enum VirtGpuCapset capset) {
    return connect(capset);
}

GLEncoder *HostConnection::glEncoder()
{
    if (!m_glEnc) {
        m_glEnc = std::make_unique<GLEncoder>(m_stream, &m_checksumHelper);
        m_glEnc->setContextAccessor(s_getGLContext);
    }
    return m_glEnc.get();
}

GL2Encoder *HostConnection::gl2Encoder()
{
    if (!m_gl2Enc) {
        m_gl2Enc = std::make_unique<GL2Encoder>(m_stream, &m_checksumHelper);
        m_gl2Enc->setContextAccessor(s_getGL2Context);
        m_gl2Enc->setNoHostError(m_noHostError);
        m_gl2Enc->setDrawCallFlushInterval(
            getDrawCallFlushIntervalFromProperty());
        m_gl2Enc->setHasAsyncUnmapBuffer(m_rcEnc->hasAsyncUnmapBuffer());
        m_gl2Enc->setHasSyncBufferData(m_rcEnc->hasSyncBufferData());
    }
    return m_gl2Enc.get();
}

ExtendedRCEncoderContext *HostConnection::rcEncoder()
{
    if (!m_rcEnc) {
        m_rcEnc = std::make_unique<ExtendedRCEncoderContext>(m_stream, &m_checksumHelper);

        ExtendedRCEncoderContext* rcEnc = m_rcEnc.get();
        rcEnc->setChecksumHelper(&m_checksumHelper);
        rcEnc->queryAndSetSyncImpl();
        rcEnc->queryAndSetDmaImpl();
        rcEnc->queryAndSetGLESMaxVersion();
        rcEnc->queryAndSetNoErrorState(m_noHostError);
        rcEnc->queryAndSetHostCompositionImpl();
        rcEnc->queryAndSetDirectMemSupport();
        rcEnc->queryAndSetVulkanSupport();
        rcEnc->queryAndSetDeferredVulkanCommandsSupport();
        rcEnc->queryAndSetVulkanNullOptionalStringsSupport();
        rcEnc->queryAndSetVulkanCreateResourcesWithRequirementsSupport();
        rcEnc->queryAndSetVulkanIgnoredHandles();
        rcEnc->queryAndSetYUVCache();
        rcEnc->queryAndSetAsyncUnmapBuffer();
        rcEnc->queryAndSetVirtioGpuNext();
        rcEnc->queryHasSharedSlotsHostMemoryAllocator();
        rcEnc->queryAndSetVulkanFreeMemorySync();
        rcEnc->queryAndSetVirtioGpuNativeSync();
        rcEnc->queryAndSetVulkanShaderFloat16Int8Support();
        rcEnc->queryAndSetVulkanAsyncQueueSubmitSupport();
        rcEnc->queryAndSetHostSideTracingSupport();
        rcEnc->queryAndSetAsyncFrameCommands();
        rcEnc->queryAndSetVulkanQueueSubmitWithCommandsSupport();
        rcEnc->queryAndSetVulkanBatchedDescriptorSetUpdateSupport();
        rcEnc->queryAndSetSyncBufferData();
        rcEnc->queryAndSetVulkanAsyncQsri();
        rcEnc->queryAndSetReadColorBufferDma();
        rcEnc->queryAndSetHWCMultiConfigs();
        rcEnc->queryAndSetHWCColorTransform();
        rcEnc->queryAndSetHWCPowerDisplayModeSupport();
        rcEnc->queryAndSetVulkanAuxCommandBufferMemory();
        rcEnc->queryVersion();

        rcEnc->rcSetPuid(rcEnc, getPuid());
    }
    return m_rcEnc.get();
}

gl_client_context_t *HostConnection::s_getGLContext()
{
    EGLThreadInfo *ti = getEGLThreadInfo();
    if (ti->hostConn) {
        return ti->hostConn->m_glEnc.get();
    }
    return NULL;
}

gl2_client_context_t *HostConnection::s_getGL2Context()
{
    EGLThreadInfo *ti = getEGLThreadInfo();
    if (ti->hostConn) {
        return ti->hostConn->m_gl2Enc.get();
    }
    return NULL;
}

void HostConnection::setVulkanFeatureInfo(void* info) {
    ExtendedRCEncoderContext* rcEnc = m_rcEnc.get();
    rcEnc->setVulkanFeatureInfo(info);
}
