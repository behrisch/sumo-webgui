#include "POILayerRhi.h"

#include <array>
#include <cstring>

#include "rhi_compat/RhiLayerCommon.h"
#include "sim/NetworkGeometry.h"

namespace {

constexpr float kHalf = 1.8f;
constexpr std::array<float, 12> kQuad = {
    -kHalf, -kHalf,  kHalf, -kHalf,  kHalf,  kHalf,
    -kHalf, -kHalf,  kHalf,  kHalf, -kHalf,  kHalf,
};

// UBO layout: mat4 proj (64) + float half_size aligned to vec4 = 16 (std140).
constexpr quint32 kUboSize = 80;

}  // namespace

POILayerRhi::POILayerRhi() = default;
POILayerRhi::~POILayerRhi() { release(); }

void POILayerRhi::release() {
    m_pipeline.reset(); m_srb.reset(); m_ubuf.reset();
    m_colVbo.reset(); m_posVbo.reset(); m_quadVbo.reset();
    m_posCapacityBytes = m_colCapacityBytes = 0;
    m_uploadQuad = true; m_initialized = false; m_rhi = nullptr;
}

void POILayerRhi::initialize(QRhi* rhi, QRhiRenderPassDescriptor* rp, int sampleCount) {
    if (m_initialized && m_rhi == rhi) return;
    release();
    m_rhi = rhi;

    m_quadVbo.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                   static_cast<quint32>(kQuad.size() * sizeof(float))));
    m_quadVbo->create();
    auto makeDyn = [&](quint32 sz) {
        auto* b = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sz);
        b->create(); return b;
    };
    m_posVbo.reset(makeDyn(8 * sizeof(float))); m_posCapacityBytes = 8 * sizeof(float);
    m_colVbo.reset(makeDyn(4));                 m_colCapacityBytes = 4;

    m_ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUboSize));
    m_ubuf->create();

    m_srb.reset(rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage |
                   QRhiShaderResourceBinding::FragmentStage,
            m_ubuf.get()),
    });
    m_srb->create();

    QShader vs = rhi_compat::loadShader(QStringLiteral(":/shaders/poi.vert.qsb"));
    QShader fs = rhi_compat::loadShader(QStringLiteral(":/shaders/poi.frag.qsb"));

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
        { 4 * sizeof(quint8), QRhiVertexInputBinding::PerInstance },
    });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2,     0 },
        { 1, 1, QRhiVertexInputAttribute::Float2,     0 },
        { 2, 2, QRhiVertexInputAttribute::UNormByte4, 0 },
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

void POILayerRhi::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    if (!ng || ng->poi_count() == 0) {
        m_instanceCount = 0;
        m_posStaging.clear(); m_colStaging.clear();
        m_uploadDirty = true;
        return;
    }
    const std::size_t n = ng->poi_count();
    m_posStaging.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        m_posStaging[i * 2]     = ng->poi_x[i];
        m_posStaging[i * 2 + 1] = ng->poi_y[i];
    }
    m_colStaging.assign(ng->poi_rgba.begin(), ng->poi_rgba.end());
    m_instanceCount = n;
    m_uploadDirty   = true;
}

void POILayerRhi::setProjection(const float* p) {
    std::memcpy(m_projStaging, p, 64);
}

void POILayerRhi::resourceUpdate(QRhiResourceUpdateBatch* batch) {
    if (!m_initialized) return;
    if (m_uploadQuad) {
        batch->uploadStaticBuffer(m_quadVbo.get(), kQuad.data());
        m_uploadQuad = false;
    }
    batch->updateDynamicBuffer(m_ubuf.get(), 0, 64, m_projStaging);
    const float half = kHalf;
    batch->updateDynamicBuffer(m_ubuf.get(), 64, 4, &half);

    if (!m_uploadDirty || m_instanceCount == 0) return;
    const std::size_t posB = m_posStaging.size() * sizeof(float);
    const std::size_t colB = m_colStaging.size();
    rhi_compat::ensureCapacity(m_rhi, m_posVbo, m_posCapacityBytes, posB, QRhiBuffer::VertexBuffer);
    rhi_compat::ensureCapacity(m_rhi, m_colVbo, m_colCapacityBytes, colB, QRhiBuffer::VertexBuffer);
    batch->updateDynamicBuffer(m_posVbo.get(), 0, static_cast<quint32>(posB), m_posStaging.data());
    batch->updateDynamicBuffer(m_colVbo.get(), 0, static_cast<quint32>(colB), m_colStaging.data());
    m_uploadDirty = false;
}

void POILayerRhi::render(QRhiCommandBuffer* cb) {
    if (!m_initialized || m_instanceCount == 0) return;
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setShaderResources();
    const QRhiCommandBuffer::VertexInput vb[] = {
        { m_quadVbo.get(), 0 }, { m_posVbo.get(), 0 }, { m_colVbo.get(), 0 },
    };
    cb->setVertexInput(0, 3, vb);
    cb->draw(6, static_cast<quint32>(m_instanceCount));
}
