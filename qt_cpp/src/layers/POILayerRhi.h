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

// POI layer: one disc per POI. Static geometry — positions+colours uploaded
// once at setGeometry; per-instance disc shaded in the fragment shader.
class POILayerRhi {
public:
    POILayerRhi();
    ~POILayerRhi();
    POILayerRhi(const POILayerRhi&)            = delete;
    POILayerRhi& operator=(const POILayerRhi&) = delete;

    void initialize(QRhi* rhi, QRhiRenderPassDescriptor* rp, int sampleCount = 1);
    void release();
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void setProjection(const float* projColMajor);
    void resourceUpdate(QRhiResourceUpdateBatch* batch);
    void render(QRhiCommandBuffer* cb);

private:
    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer>                 m_quadVbo;
    std::unique_ptr<QRhiBuffer>                 m_posVbo;
    std::unique_ptr<QRhiBuffer>                 m_colVbo;
    std::unique_ptr<QRhiBuffer>                 m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pipeline;

    std::vector<float>   m_posStaging;
    std::vector<quint8>  m_colStaging;
    float                m_projStaging[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    std::size_t m_instanceCount    = 0;
    std::size_t m_posCapacityBytes = 0;
    std::size_t m_colCapacityBytes = 0;
    bool m_uploadQuad   = true;
    bool m_uploadDirty  = false;
    bool m_initialized  = false;
};
