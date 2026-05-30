#pragma once

#include <memory>
#include <vector>

#include <QtCore/qglobal.h>

#include "sim/SimSnapshot.h"

struct NetworkGeometry;

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;
class QRhiShaderResourceBindings;
QT_END_NAMESPACE

// TLS bar layer. Positions are static (from NetworkGeometry); per-instance
// colors come from SimSnapshot::tls_states.
class TLSLayerRhi {
public:
    TLSLayerRhi();
    ~TLSLayerRhi();
    TLSLayerRhi(const TLSLayerRhi&)            = delete;
    TLSLayerRhi& operator=(const TLSLayerRhi&) = delete;

    void initialize(QRhi* rhi, QRhiRenderPassDescriptor* rp, int sampleCount = 1);
    void release();
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void setSnapshot(const SimSnapshotPtr& snap);
    void setProjection(const float* projColMajor);
    void resourceUpdate(QRhiResourceUpdateBatch* batch);
    void render(QRhiCommandBuffer* cb);

private:
    void rebuildColors();

    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer>                 m_quadVbo;
    std::unique_ptr<QRhiBuffer>                 m_posVbo;
    std::unique_ptr<QRhiBuffer>                 m_tanVbo;
    std::unique_ptr<QRhiBuffer>                 m_widthVbo;
    std::unique_ptr<QRhiBuffer>                 m_colVbo;
    std::unique_ptr<QRhiBuffer>                 m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pipeline;

    std::shared_ptr<NetworkGeometry> m_ng;
    SimSnapshotPtr                   m_snap;

    std::vector<float>   m_posStaging;
    std::vector<float>   m_tanStaging;
    std::vector<float>   m_widStaging;
    std::vector<quint8>  m_colStaging;
    float                m_projStaging[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    std::size_t m_instanceCount    = 0;
    std::size_t m_posCapacityBytes = 0;
    std::size_t m_tanCapacityBytes = 0;
    std::size_t m_widCapacityBytes = 0;
    std::size_t m_colCapacityBytes = 0;
    bool m_uploadQuad   = true;
    bool m_posDirty     = false;
    bool m_colDirty     = false;
    bool m_initialized  = false;
};
