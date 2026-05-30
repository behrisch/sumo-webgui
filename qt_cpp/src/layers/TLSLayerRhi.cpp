#include "TLSLayerRhi.h"

#include <array>
#include <cstring>

#include "rhi_compat/RhiLayerCommon.h"
#include "sim/NetworkGeometry.h"

namespace {

constexpr float kBarLen  = 0.9f;
// Place the TLS bar exactly at the lane end (i.e. at the stop line), so
// signals and uncontrolled stop bars share the same on-screen position.
constexpr float kBarBack = 0.0f;

constexpr std::array<float, 12> kQuadBase = {
    -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,
};

struct Rgba { quint8 r, g, b, a; };

Rgba colorForStateChar(char c) {
    switch (c) {
        case 'r': case 'R': return {220,  40,  40, 255};
        case 'y': case 'Y': return {235, 200,  40, 255};
        case 'g':           return { 50, 180,  90, 255};
        case 'G':           return { 80, 230, 100, 255};
        case 's':           return {220,  40,  40, 255};
        case 'u':           return {235, 130,  40, 255};
        case 'o': case 'O': return { 90,  90,  90, 255};
        default:            return {120, 120, 120, 255};
    }
}

}  // namespace

TLSLayerRhi::TLSLayerRhi()  = default;
TLSLayerRhi::~TLSLayerRhi() { release(); }

void TLSLayerRhi::release() {
    m_pipeline.reset(); m_srb.reset(); m_ubuf.reset();
    m_colVbo.reset(); m_widthVbo.reset(); m_tanVbo.reset();
    m_posVbo.reset(); m_quadVbo.reset();
    m_posCapacityBytes = m_tanCapacityBytes = m_widCapacityBytes = m_colCapacityBytes = 0;
    m_uploadQuad = true; m_initialized = false; m_rhi = nullptr;
}

void TLSLayerRhi::initialize(QRhi* rhi, QRhiRenderPassDescriptor* rp, int sampleCount) {
    if (m_initialized && m_rhi == rhi) return;
    release();
    m_rhi = rhi;

    m_quadVbo.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                   static_cast<quint32>(kQuadBase.size() * sizeof(float))));
    m_quadVbo->create();
    auto makeDyn = [&](quint32 sz) {
        auto* b = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sz);
        b->create(); return b;
    };
    m_posVbo.reset(makeDyn(8 * sizeof(float)));   m_posCapacityBytes = 8 * sizeof(float);
    m_tanVbo.reset(makeDyn(8 * sizeof(float)));   m_tanCapacityBytes = 8 * sizeof(float);
    m_widthVbo.reset(makeDyn(4 * sizeof(float))); m_widCapacityBytes = 4 * sizeof(float);
    m_colVbo.reset(makeDyn(4));                   m_colCapacityBytes = 4;

    m_ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64));
    m_ubuf->create();

    m_srb.reset(rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage |
                   QRhiShaderResourceBinding::FragmentStage,
            m_ubuf.get()),
    });
    m_srb->create();

    QShader vs = rhi_compat::loadShader(QStringLiteral(":/shaders/tls.vert.qsb"));
    QShader fs = rhi_compat::loadShader(QStringLiteral(":/shaders/tls.frag.qsb"));

    m_pipeline.reset(rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_pipeline->setDepthTest(false); m_pipeline->setDepthWrite(false);
    m_pipeline->setSampleCount(sampleCount);
    m_pipeline->setTargetBlends({ rhi_compat::alphaBlend() });

    QRhiVertexInputLayout layout;
    layout.setBindings({
        { 2 * sizeof(float),  QRhiVertexInputBinding::PerVertex },
        { 2 * sizeof(float),  QRhiVertexInputBinding::PerInstance },
        { 2 * sizeof(float),  QRhiVertexInputBinding::PerInstance },
        { sizeof(float),      QRhiVertexInputBinding::PerInstance },
        { 4 * sizeof(quint8), QRhiVertexInputBinding::PerInstance },
    });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2,     0 },
        { 1, 1, QRhiVertexInputAttribute::Float2,     0 },
        { 2, 2, QRhiVertexInputAttribute::Float2,     0 },
        { 3, 3, QRhiVertexInputAttribute::Float,      0 },
        { 4, 4, QRhiVertexInputAttribute::UNormByte4, 0 },
    });
    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vs }, { QRhiShaderStage::Fragment, fs },
    });
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(rp);
    m_pipeline->create();

    m_uploadQuad = true;
    m_initialized = true;
}

void TLSLayerRhi::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    if (!ng || ng->tls_marker_count() == 0) {
        m_instanceCount = 0;
        m_posStaging.clear(); m_tanStaging.clear(); m_widStaging.clear();
        m_posDirty = true; return;
    }
    const std::size_t n = ng->tls_marker_count();
    m_posStaging.resize(n * 2);
    m_tanStaging.resize(n * 2);
    m_widStaging.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        m_posStaging[i * 2]     = ng->tls_x[i] - ng->tls_dx[i] * kBarBack;
        m_posStaging[i * 2 + 1] = ng->tls_y[i] - ng->tls_dy[i] * kBarBack;
        m_tanStaging[i * 2]     = ng->tls_dx[i];
        m_tanStaging[i * 2 + 1] = ng->tls_dy[i];
        m_widStaging[i]         = ng->tls_w[i];
    }
    m_instanceCount = n;
    m_posDirty = true;
    m_colDirty = true;
    rebuildColors();
}

void TLSLayerRhi::setSnapshot(const SimSnapshotPtr& snap) {
    m_snap = snap;
    m_colDirty = true;
    rebuildColors();
}

void TLSLayerRhi::rebuildColors() {
    if (!m_ng || m_instanceCount == 0) {
        m_colStaging.clear();
        return;
    }
    const std::size_t n = m_ng->tls_marker_count();
    m_colStaging.resize(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        Rgba c{120, 120, 120, 255};
        if (m_snap) {
            const auto it = m_snap->tls_states.find(m_ng->tls_ids[i]);
            if (it != m_snap->tls_states.end()) {
                const std::uint32_t idx = m_ng->tls_state_index[i];
                if (idx < it->second.size())
                    c = colorForStateChar(it->second[idx]);
            }
        }
        m_colStaging[i * 4 + 0] = c.r;
        m_colStaging[i * 4 + 1] = c.g;
        m_colStaging[i * 4 + 2] = c.b;
        m_colStaging[i * 4 + 3] = c.a;
    }
}

void TLSLayerRhi::setProjection(const float* p) { std::memcpy(m_projStaging, p, 64); }

void TLSLayerRhi::resourceUpdate(QRhiResourceUpdateBatch* batch) {
    if (!m_initialized) return;
    if (m_uploadQuad) {
        std::array<float, 12> q = kQuadBase;
        for (int i = 0; i < 6; ++i) q[i * 2] *= kBarLen;
        batch->uploadStaticBuffer(m_quadVbo.get(), q.data());
        m_uploadQuad = false;
    }
    batch->updateDynamicBuffer(m_ubuf.get(), 0, 64, m_projStaging);

    if (m_posDirty && !m_posStaging.empty()) {
        const std::size_t pB = m_posStaging.size() * sizeof(float);
        const std::size_t tB = m_tanStaging.size() * sizeof(float);
        const std::size_t wB = m_widStaging.size() * sizeof(float);
        rhi_compat::ensureCapacity(m_rhi, m_posVbo,   m_posCapacityBytes, pB, QRhiBuffer::VertexBuffer);
        rhi_compat::ensureCapacity(m_rhi, m_tanVbo,   m_tanCapacityBytes, tB, QRhiBuffer::VertexBuffer);
        rhi_compat::ensureCapacity(m_rhi, m_widthVbo, m_widCapacityBytes, wB, QRhiBuffer::VertexBuffer);
        batch->updateDynamicBuffer(m_posVbo.get(),   0, static_cast<quint32>(pB), m_posStaging.data());
        batch->updateDynamicBuffer(m_tanVbo.get(),   0, static_cast<quint32>(tB), m_tanStaging.data());
        batch->updateDynamicBuffer(m_widthVbo.get(), 0, static_cast<quint32>(wB), m_widStaging.data());
        m_posDirty = false;
    }
    if (m_colDirty && !m_colStaging.empty()) {
        const std::size_t cB = m_colStaging.size();
        rhi_compat::ensureCapacity(m_rhi, m_colVbo, m_colCapacityBytes, cB, QRhiBuffer::VertexBuffer);
        batch->updateDynamicBuffer(m_colVbo.get(), 0, static_cast<quint32>(cB), m_colStaging.data());
        m_colDirty = false;
    }
}

void TLSLayerRhi::render(QRhiCommandBuffer* cb) {
    if (!m_initialized || m_instanceCount == 0) return;
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setShaderResources();
    const QRhiCommandBuffer::VertexInput vb[] = {
        { m_quadVbo.get(),  0 }, { m_posVbo.get(),  0 },
        { m_tanVbo.get(),   0 }, { m_widthVbo.get(),0 },
        { m_colVbo.get(),   0 },
    };
    cb->setVertexInput(0, 5, vb);
    cb->draw(6, static_cast<quint32>(m_instanceCount));
}
