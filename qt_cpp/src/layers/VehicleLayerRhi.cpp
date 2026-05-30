#include "VehicleLayerRhi.h"

#include <array>
#include <cstring>

#include <QtCore/QFile>
#include "rhi_compat/rhi_compat.h"

namespace {

// Quad in local vehicle frame: length along +x, width along +y. Same geometry
// as the OpenGL VehicleLayer.
constexpr float kHalfLen = 2.5f;
constexpr float kHalfWid = 1.0f;
constexpr std::array<float, 12> kQuad = {
    -kHalfLen, -kHalfWid,
     kHalfLen, -kHalfWid,
     kHalfLen,  kHalfWid,
    -kHalfLen, -kHalfWid,
     kHalfLen,  kHalfWid,
    -kHalfLen,  kHalfWid,
};

QShader loadShader(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("VehicleLayerRhi: failed to open shader %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

}  // namespace

VehicleLayerRhi::VehicleLayerRhi() = default;
VehicleLayerRhi::~VehicleLayerRhi() { release(); }

void VehicleLayerRhi::release() {
    m_pipeline.reset();
    m_srb.reset();
    m_ubuf.reset();
    m_colVbo.reset();
    m_angVbo.reset();
    m_posVbo.reset();
    m_quadVbo.reset();
    m_posCapacityBytes = m_angCapacityBytes = m_colCapacityBytes = 0;
    m_uploadQuad   = true;
    m_initialized  = false;
    m_rhi = nullptr;
}

void VehicleLayerRhi::initialize(QRhi* rhi,
                                 QRhiRenderPassDescriptor* rp,
                                 int sampleCount) {
    if (m_initialized && m_rhi == rhi) return;
    release();
    m_rhi = rhi;

    // Static quad buffer (immutable).
    m_quadVbo.reset(rhi->newBuffer(QRhiBuffer::Immutable,
                                   QRhiBuffer::VertexBuffer,
                                   static_cast<quint32>(kQuad.size() * sizeof(float))));
    m_quadVbo->create();

    // Per-instance buffers (Dynamic so we can resize/upload every frame).
    // Start with a small placeholder size; grow on demand.
    auto makeDyn = [&](quint32 sz) {
        auto* b = rhi->newBuffer(QRhiBuffer::Dynamic,
                                 QRhiBuffer::VertexBuffer,
                                 sz);
        b->create();
        return b;
    };
    m_posVbo.reset(makeDyn(8 * sizeof(float)));
    m_angVbo.reset(makeDyn(sizeof(float)));
    m_colVbo.reset(makeDyn(4));
    m_posCapacityBytes = 8 * sizeof(float);
    m_angCapacityBytes = sizeof(float);
    m_colCapacityBytes = 4;

    // Uniform buffer: mat4 proj. std140 = 64 bytes.
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

    // Load shaders (baked at build time by qsb, accessed via Qt Resource).
    QShader vs = loadShader(QStringLiteral(":/shaders/vehicle.vert.qsb"));
    QShader fs = loadShader(QStringLiteral(":/shaders/vehicle.frag.qsb"));

    m_pipeline.reset(rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_pipeline->setDepthTest(false);
    m_pipeline->setDepthWrite(false);
    m_pipeline->setSampleCount(sampleCount);

    QRhiVertexInputLayout layout;
    layout.setBindings({
        { 2 * sizeof(float),   QRhiVertexInputBinding::PerVertex },    // 0: quad
        { 2 * sizeof(float),   QRhiVertexInputBinding::PerInstance },  // 1: pos
        { sizeof(float),       QRhiVertexInputBinding::PerInstance },  // 2: angle
        { 4 * sizeof(quint8),  QRhiVertexInputBinding::PerInstance },  // 3: rgba
    });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2,    0 },
        { 1, 1, QRhiVertexInputAttribute::Float2,    0 },
        { 2, 2, QRhiVertexInputAttribute::Float,     0 },
        { 3, 3, QRhiVertexInputAttribute::UNormByte4, 0 },
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

void VehicleLayerRhi::setSnapshot(const SimSnapshotPtr& snap) {
    if (!snap || snap->vehicle_count() == 0) {
        m_instanceCount = 0;
        m_posStaging.clear();
        m_angStaging.clear();
        m_colStaging.clear();
        m_uploadDirty = true;
        return;
    }
    const std::size_t n = snap->vehicle_count();

    // Narrow f64 (x,y,z stride 24) -> f32 (x,y stride 8). Cheap; small alloc
    // on growth only (vectors keep capacity across frames).
    m_posStaging.resize(n * 2);
    const auto* src = reinterpret_cast<const double*>(snap->veh_positions.data());
    for (std::size_t i = 0; i < n; ++i) {
        m_posStaging[i * 2 + 0] = static_cast<float>(src[i * 3 + 0]);
        m_posStaging[i * 2 + 1] = static_cast<float>(src[i * 3 + 1]);
    }

    // Angles are already f32; copy raw bytes.
    m_angStaging.resize(n);
    std::memcpy(m_angStaging.data(),
                snap->veh_angles.data(),
                std::min(snap->veh_angles.size(), n * sizeof(float)));

    m_colStaging.assign(snap->rgba.begin(), snap->rgba.end());

    m_instanceCount = n;
    m_uploadDirty   = true;
}

namespace {
// Grow `buf` to at least `wanted` bytes; updates the capacity tracker. Returns
// true if the buffer was rebuilt (caller must recreate ShaderResourceBindings
// references — not needed for vertex buffers).
bool ensureCapacity(QRhi* rhi,
                    std::unique_ptr<QRhiBuffer>& buf,
                    std::size_t& cap,
                    std::size_t wanted,
                    QRhiBuffer::UsageFlags usage) {
    if (wanted <= cap) return false;
    // Grow with some slack to avoid every-step rebuilds.
    const std::size_t newCap = std::max<std::size_t>(wanted, cap * 2);
    buf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, usage,
                             static_cast<quint32>(newCap)));
    buf->create();
    cap = newCap;
    return true;
}
}  // namespace

void VehicleLayerRhi::resourceUpdate(QRhiResourceUpdateBatch* batch) {
    if (!m_initialized) return;

    if (m_uploadQuad) {
        batch->uploadStaticBuffer(m_quadVbo.get(), kQuad.data());
        m_uploadQuad = false;
    }

    // Upload projection matrix every frame; cheap (64 B).
    batch->updateDynamicBuffer(m_ubuf.get(), 0, 64, m_projStaging);

    if (!m_uploadDirty || m_instanceCount == 0) return;

    const std::size_t posBytes = m_posStaging.size() * sizeof(float);
    const std::size_t angBytes = m_angStaging.size() * sizeof(float);
    const std::size_t colBytes = m_colStaging.size();

    ensureCapacity(m_rhi, m_posVbo, m_posCapacityBytes, posBytes,
                   QRhiBuffer::VertexBuffer);
    ensureCapacity(m_rhi, m_angVbo, m_angCapacityBytes, angBytes,
                   QRhiBuffer::VertexBuffer);
    ensureCapacity(m_rhi, m_colVbo, m_colCapacityBytes, colBytes,
                   QRhiBuffer::VertexBuffer);

    batch->updateDynamicBuffer(m_posVbo.get(), 0,
                               static_cast<quint32>(posBytes),
                               m_posStaging.data());
    batch->updateDynamicBuffer(m_angVbo.get(), 0,
                               static_cast<quint32>(angBytes),
                               m_angStaging.data());
    batch->updateDynamicBuffer(m_colVbo.get(), 0,
                               static_cast<quint32>(colBytes),
                               m_colStaging.data());

    m_uploadDirty = false;
}

void VehicleLayerRhi::setProjection(const float* projectionMatColMajor) {
    std::memcpy(m_projStaging, projectionMatColMajor, 16 * sizeof(float));
}

void VehicleLayerRhi::render(QRhiCommandBuffer* cb) {
    if (!m_initialized || m_instanceCount == 0) return;

    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setShaderResources();

    const QRhiCommandBuffer::VertexInput bindings[] = {
        { m_quadVbo.get(), 0 },
        { m_posVbo.get(),  0 },
        { m_angVbo.get(),  0 },
        { m_colVbo.get(),  0 },
    };
    cb->setVertexInput(0, 4, bindings);
    cb->draw(6, static_cast<quint32>(m_instanceCount));
}
