#pragma once

// Pipeline + SRB + UBO + projection-uniform plumbing for the static
// "per-vertex {pos:Float2, rgba:UNormByte4}" layers. Every static fill /
// outline / band layer in NetworkView uses this — even "uniform color"
// layers bake the constant rgba into each vertex at staging time, so we
// only ship one shader pair.

#include <cstring>
#include <memory>

#include <QtCore/qglobal.h>
#include "rhi_compat/RhiLayerCommon.h"

// Interleaved variant: one binding stride 12 bytes (8 pos + 4 rgba).
class TrisColorInterleavedPass {
public:
    void release() {
        m_pipeline.reset(); m_srb.reset(); m_ubuf.reset();
        m_rhi = nullptr; m_initialized = false;
    }

    bool init(QRhi* rhi, QRhiRenderPassDescriptor* rp, int sampleCount,
              QRhiGraphicsPipeline::Topology topo = QRhiGraphicsPipeline::Triangles) {
        if (m_initialized && m_rhi == rhi && m_topo == topo) return true;
        release();
        m_rhi = rhi; m_topo = topo;

        m_ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic,
                                    QRhiBuffer::UniformBuffer, 64));
        m_ubuf->create();
        m_srb.reset(rhi->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                m_ubuf.get()),
        });
        m_srb->create();

        QShader vs = rhi_compat::loadShader(QStringLiteral(":/shaders/tris_color.vert.qsb"));
        QShader fs = rhi_compat::loadShader(QStringLiteral(":/shaders/tris_color.frag.qsb"));

        m_pipeline.reset(rhi->newGraphicsPipeline());
        m_pipeline->setTopology(topo);
        m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
        m_pipeline->setDepthTest(false); m_pipeline->setDepthWrite(false);
        m_pipeline->setSampleCount(sampleCount);
        m_pipeline->setTargetBlends({ rhi_compat::alphaBlend() });

        QRhiVertexInputLayout layout;
        layout.setBindings({
            { 12, QRhiVertexInputBinding::PerVertex },
        });
        layout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float2,     0 },
            { 0, 1, QRhiVertexInputAttribute::UNormByte4, 8 },
        });
        m_pipeline->setVertexInputLayout(layout);
        m_pipeline->setShaderStages({
            { QRhiShaderStage::Vertex, vs }, { QRhiShaderStage::Fragment, fs },
        });
        m_pipeline->setShaderResourceBindings(m_srb.get());
        m_pipeline->setRenderPassDescriptor(rp);
        m_pipeline->create();

        m_initialized = true;
        return true;
    }

    void setProjection(const float* p) { std::memcpy(m_proj, p, 64); }
    void uploadUbo(QRhiResourceUpdateBatch* batch) const {
        if (!m_initialized) return;
        batch->updateDynamicBuffer(m_ubuf.get(), 0, 64, m_proj);
    }

    void drawVB(QRhiCommandBuffer* cb, QRhiBuffer* vbo,
                quint32 vertexCount, quint32 firstVertex = 0) const {
        if (!m_initialized || vertexCount == 0) return;
        cb->setGraphicsPipeline(m_pipeline.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput vb[] = { { vbo, 0 } };
        cb->setVertexInput(0, 1, vb);
        cb->draw(vertexCount, 1, firstVertex);
    }

    bool initialized() const { return m_initialized; }

private:
    QRhi* m_rhi = nullptr;
    QRhiGraphicsPipeline::Topology m_topo = QRhiGraphicsPipeline::Triangles;
    std::unique_ptr<QRhiBuffer>                 m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pipeline;
    float m_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool  m_initialized = false;
};

// Helper: pack {x, y, r, g, b, a} into a 12-byte vertex.
struct TrisColorVertex {
    float x, y;
    quint8 r, g, b, a;
};
static_assert(sizeof(TrisColorVertex) == 12);

inline void pushTri(std::vector<TrisColorVertex>& v,
                    float ax, float ay, float bx, float by,
                    float cx, float cy,
                    quint8 r, quint8 g, quint8 b, quint8 a) {
    v.push_back({ax, ay, r, g, b, a});
    v.push_back({bx, by, r, g, b, a});
    v.push_back({cx, cy, r, g, b, a});
}

inline void pushQuad(std::vector<TrisColorVertex>& v,
                     float ax, float ay, float bx, float by,
                     float cx, float cy, float dx, float dy,
                     quint8 r, quint8 g, quint8 b, quint8 a) {
    pushTri(v, ax, ay, bx, by, cx, cy, r, g, b, a);
    pushTri(v, ax, ay, cx, cy, dx, dy, r, g, b, a);
}
