#include "PersonLayerRhi.h"

#include <array>
#include <cstring>

#include <QtCore/QFile>
#include "rhi_compat/rhi_compat.h"

namespace {

// 1.2 m square (matches OpenGL PersonLayer's kHalf = 0.6 m).
constexpr float kHalf = 0.6f;
constexpr std::array<float, 12> kQuad = {
    -kHalf, -kHalf,
     kHalf, -kHalf,
     kHalf,  kHalf,
    -kHalf, -kHalf,
     kHalf,  kHalf,
    -kHalf,  kHalf,
};

QShader loadShader(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("PersonLayerRhi: failed to open shader %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

bool ensureCapacity(QRhi* rhi,
                    std::unique_ptr<QRhiBuffer>& buf,
                    std::size_t& cap,
                    std::size_t wanted,
                    QRhiBuffer::UsageFlags usage) {
    if (wanted <= cap) return false;
    const std::size_t newCap = std::max<std::size_t>(wanted, cap * 2);
    buf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, usage,
                             static_cast<quint32>(newCap)));
    buf->create();
    cap = newCap;
    return true;
}

}  // namespace

PersonLayerRhi::PersonLayerRhi() = default;
PersonLayerRhi::~PersonLayerRhi() { release(); }

void PersonLayerRhi::release() {
    m_pipeline.reset();
    m_srb.reset();
    m_ubuf.reset();
    m_colVbo.reset();
    m_posVbo.reset();
    m_quadVbo.reset();
    m_posCapacityBytes = m_colCapacityBytes = 0;
    m_uploadQuad  = true;
    m_initialized = false;
    m_rhi = nullptr;
}

void PersonLayerRhi::initialize(QRhi* rhi,
                                QRhiRenderPassDescriptor* rp,
                                int sampleCount) {
    if (m_initialized && m_rhi == rhi) return;
    release();
    m_rhi = rhi;

    m_quadVbo.reset(rhi->newBuffer(QRhiBuffer::Immutable,
                                   QRhiBuffer::VertexBuffer,
                                   static_cast<quint32>(kQuad.size() * sizeof(float))));
    m_quadVbo->create();

    auto makeDyn = [&](quint32 sz) {
        auto* b = rhi->newBuffer(QRhiBuffer::Dynamic,
                                 QRhiBuffer::VertexBuffer, sz);
        b->create();
        return b;
    };
    m_posVbo.reset(makeDyn(8 * sizeof(float)));
    m_colVbo.reset(makeDyn(4));
    m_posCapacityBytes = 8 * sizeof(float);
    m_colCapacityBytes = 4;

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

    QShader vs = loadShader(QStringLiteral(":/shaders/person.vert.qsb"));
    QShader fs = loadShader(QStringLiteral(":/shaders/person.frag.qsb"));

    m_pipeline.reset(rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_pipeline->setDepthTest(false);
    m_pipeline->setDepthWrite(false);
    m_pipeline->setSampleCount(sampleCount);

    QRhiVertexInputLayout layout;
    layout.setBindings({
        { 2 * sizeof(float),  QRhiVertexInputBinding::PerVertex },    // 0: quad
        { 2 * sizeof(float),  QRhiVertexInputBinding::PerInstance },  // 1: pos
        { 4 * sizeof(quint8), QRhiVertexInputBinding::PerInstance },  // 2: rgba
    });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2,     0 },
        { 1, 1, QRhiVertexInputAttribute::Float2,     0 },
        { 2, 2, QRhiVertexInputAttribute::UNormByte4, 0 },
    });

    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex,   vs },
        { QRhiShaderStage::Fragment, fs },
    });
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(rp);
    m_pipeline->create();

    m_uploadQuad  = true;
    m_initialized = true;
}

void PersonLayerRhi::setSnapshot(const SimSnapshotPtr& snap) {
    if (!snap || snap->person_count() == 0) {
        m_instanceCount = 0;
        m_posStaging.clear();
        m_colStaging.clear();
        m_uploadDirty = true;
        return;
    }
    const std::size_t n = snap->person_count();

    m_posStaging.resize(n * 2);
    const auto* src = reinterpret_cast<const double*>(snap->agent_positions.data());
    for (std::size_t i = 0; i < n; ++i) {
        m_posStaging[i * 2 + 0] = static_cast<float>(src[i * 3 + 0]);
        m_posStaging[i * 2 + 1] = static_cast<float>(src[i * 3 + 1]);
    }

    m_colStaging.assign(snap->person_rgba.begin(), snap->person_rgba.end());

    m_instanceCount = n;
    m_uploadDirty   = true;
}

void PersonLayerRhi::resourceUpdate(QRhiResourceUpdateBatch* batch) {
    if (!m_initialized) return;

    if (m_uploadQuad) {
        batch->uploadStaticBuffer(m_quadVbo.get(), kQuad.data());
        m_uploadQuad = false;
    }

    batch->updateDynamicBuffer(m_ubuf.get(), 0, 64, m_projStaging);

    if (!m_uploadDirty || m_instanceCount == 0) return;

    const std::size_t posBytes = m_posStaging.size() * sizeof(float);
    const std::size_t colBytes = m_colStaging.size();

    ensureCapacity(m_rhi, m_posVbo, m_posCapacityBytes, posBytes,
                   QRhiBuffer::VertexBuffer);
    ensureCapacity(m_rhi, m_colVbo, m_colCapacityBytes, colBytes,
                   QRhiBuffer::VertexBuffer);

    batch->updateDynamicBuffer(m_posVbo.get(), 0,
                               static_cast<quint32>(posBytes),
                               m_posStaging.data());
    batch->updateDynamicBuffer(m_colVbo.get(), 0,
                               static_cast<quint32>(colBytes),
                               m_colStaging.data());

    m_uploadDirty = false;
}

void PersonLayerRhi::setProjection(const float* projectionMatColMajor) {
    std::memcpy(m_projStaging, projectionMatColMajor, 16 * sizeof(float));
}

void PersonLayerRhi::render(QRhiCommandBuffer* cb) {
    if (!m_initialized || m_instanceCount == 0) return;

    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setShaderResources();

    const QRhiCommandBuffer::VertexInput bindings[] = {
        { m_quadVbo.get(), 0 },
        { m_posVbo.get(),  0 },
        { m_colVbo.get(),  0 },
    };
    cb->setVertexInput(0, 3, bindings);
    cb->draw(6, static_cast<quint32>(m_instanceCount));
}
