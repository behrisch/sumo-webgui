#pragma once

#include <memory>
#include <vector>

#include <QtCore/qglobal.h>

#include "sim/SimSnapshot.h"

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;
class QRhiShaderResourceBindings;
QT_END_NAMESPACE

// QRhi reimplementation of the person layer. Renders one instanced square per
// agent (persons + containers — they share the agent_* columns in
// SimSnapshot). No per-instance angle; colour-by-type only. Mirrors the
// OpenGL PersonLayer's behaviour.
//
// Like VehicleLayerRhi, this narrows the f64 position column to f32 in
// setSnapshot() because QRhi has no Double2 vertex-attribute format.
class PersonLayerRhi {
public:
    PersonLayerRhi();
    ~PersonLayerRhi();

    PersonLayerRhi(const PersonLayerRhi&)            = delete;
    PersonLayerRhi& operator=(const PersonLayerRhi&) = delete;

    void initialize(QRhi* rhi,
                    QRhiRenderPassDescriptor* rp,
                    int sampleCount = 1);
    void release();

    void setSnapshot(const SimSnapshotPtr& snap);
    void resourceUpdate(QRhiResourceUpdateBatch* batch);
    void setProjection(const float* projectionMatColMajor);
    void render(QRhiCommandBuffer* cb);

    std::size_t instanceCount() const { return m_instanceCount; }

private:
    QRhi* m_rhi = nullptr;

    std::unique_ptr<QRhiBuffer>                 m_quadVbo;
    std::unique_ptr<QRhiBuffer>                 m_posVbo;
    std::unique_ptr<QRhiBuffer>                 m_colVbo;
    std::unique_ptr<QRhiBuffer>                 m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pipeline;

    std::vector<float>   m_posStaging;   // (x,y) per agent, narrowed from f64
    std::vector<uint8_t> m_colStaging;   // copy of rgba8
    float                m_projStaging[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    std::size_t m_instanceCount    = 0;
    std::size_t m_posCapacityBytes = 0;
    std::size_t m_colCapacityBytes = 0;

    bool m_uploadQuad  = true;
    bool m_uploadDirty = false;
    bool m_initialized = false;
};
