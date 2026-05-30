#include "VehicleLayerRhi.h"

#include <array>
#include <cmath>
#include <cstring>

#include <QtCore/QFile>
#include "rhi_compat/rhi_compat.h"

namespace {

// Build a triangle list for the requested body shape in the unit local frame:
//   x ∈ [-1, 0] (rear at -1, front at 0; SUMO anchor = front-bumper centre)
//   y ∈ [-1, +1] (half-width 1 m; shader scales x by per-instance length)
//
// Each vertex is interleaved as { float x, float y, uint8 tint, _, _, _ } —
// 12 bytes / vertex. The 3 padding bytes round the stride to a multiple of
// 4 which keeps every backend happy (Vulkan/Metal/D3D12). The shader reads
// the tint as a UNormByte4 vec4 and multiplies the per-instance color by
// tint.r so 255 = body color, 128 = half, 0 = black (windshield).
//
// All shapes are emitted CCW so a future depth/winding state would treat
// them uniformly; QRhi cull mode is None so winding is currently irrelevant.
struct V { float x, y; std::uint8_t t, _a, _b, _c; };
static_assert(sizeof(V) == 12, "VehicleLayerRhi vertex must be 12 bytes");

void buildShapeMesh(VehicleLayerRhi::Shape s, std::vector<std::byte>& out) {
    std::vector<V> verts;
    auto push = [&](float x, float y, std::uint8_t t) {
        verts.push_back(V{x, y, t, 0, 0, 0});
    };
    auto tri = [&](float ax, float ay, float bx, float by, float cx, float cy,
                   std::uint8_t t) {
        push(ax, ay, t); push(bx, by, t); push(cx, cy, t);
    };
    auto fan = [&](const float (*pts)[2], int n, std::uint8_t t) {
        for (int i = 1; i < n - 1; ++i) {
            tri(pts[0][0], pts[0][1],
                pts[i][0], pts[i][1],
                pts[i + 1][0], pts[i + 1][1], t);
        }
    };

    switch (s) {
    case VehicleLayerRhi::Shape::Rectangle: {
        tri(-1.f, -1.f,  0.f, -1.f,  0.f,  1.f, 255);
        tri(-1.f, -1.f,  0.f,  1.f, -1.f,  1.f, 255);
        break;
    }
    case VehicleLayerRhi::Shape::Triangle: {
        tri(-1.f, -1.f,  0.f,  0.f, -1.f,  1.f, 255);
        break;
    }
    case VehicleLayerRhi::Shape::Car: {
        // ---- 1. Body polygon (per-instance color, tint=255) --------------
        // ecal _CAR_BODY transposed to our axes:
        //   ecal (px, py) -> our (-px, +py).
        // Centroid first (anchor for the fan), then the outline CCW.
        static const float body[][2] = {
            { -0.50f,  0.00f},  // centroid (mid-body)
            {  0.00f,  0.00f},  // front bumper centre
            {  0.00f,  0.30f},
            { -0.08f,  0.44f},
            { -0.25f,  0.50f},
            { -0.95f,  0.50f},
            { -1.00f,  0.40f},
            { -1.00f, -0.40f},
            { -0.95f, -0.50f},
            { -0.25f, -0.50f},
            { -0.08f, -0.44f},
            {  0.00f, -0.30f},
            {  0.00f,  0.00f},  // close
        };
        fan(body, int(sizeof(body) / sizeof(body[0])), 255);

        // ---- 2. Darker front-bumper overlay (tint=128) ------------------
        // ecal _CAR_BODY_FRONT, same transposition.
        static const float front[][2] = {
            { -0.10f,  0.00f},   // centroid
            { -0.025f, 0.00f},
            { -0.025f, 0.25f},
            { -0.27f,  0.40f},
            { -0.27f, -0.40f},
            { -0.025f,-0.25f},
            { -0.025f, 0.00f},   // close
        };
        fan(front, int(sizeof(front) / sizeof(front[0])), 128);

        // ---- 3. Windshield strip (tint=0 -> black) ----------------------
        // ecal _CAR_WINDSHIELD.
        static const float windshield[][2] = {
            { -0.35f,  0.00f},   // centroid
            { -0.30f,  0.00f},
            { -0.30f,  0.40f},
            { -0.43f,  0.30f},
            { -0.43f, -0.30f},
            { -0.30f, -0.40f},
            { -0.30f,  0.00f},   // close
        };
        fan(windshield, int(sizeof(windshield) / sizeof(windshield[0])), 0);
        break;
    }
    case VehicleLayerRhi::Shape::Circle: {
        const int kSeg = 24;
        const float cx = -0.5f, cy = 0.0f, r = 0.5f;
        for (int i = 0; i < kSeg; ++i) {
            const float a0 = (2.f * float(M_PI) * i) / kSeg;
            const float a1 = (2.f * float(M_PI) * (i + 1)) / kSeg;
            tri(cx, cy,
                cx + r * std::cos(a0), cy + r * std::sin(a0),
                cx + r * std::cos(a1), cy + r * std::sin(a1), 255);
        }
        break;
    }
    }

    out.resize(verts.size() * sizeof(V));
    if (!verts.empty()) std::memcpy(out.data(), verts.data(), out.size());
}

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
    m_lenVbo.reset();
    m_angVbo.reset();
    m_posVbo.reset();
    m_quadVbo.reset();
    m_posCapacityBytes = m_angCapacityBytes = m_colCapacityBytes = m_lenCapacityBytes = 0;
    m_shapeCapacityBytes = 0;
    m_uploadQuad   = true;
    m_initialized  = false;
    m_rhi = nullptr;
}

void VehicleLayerRhi::setShape(Shape s) {
    if (m_shape == s && !m_shapeVerts.empty()) return;
    m_shape = s;
    buildShapeMesh(m_shape, m_shapeVerts);
    m_shapeVertCount = m_shapeVerts.size() / sizeof(V);
    m_uploadQuad = true;
}

void VehicleLayerRhi::initialize(QRhi* rhi,
                                 QRhiRenderPassDescriptor* rp,
                                 int sampleCount) {
    if (m_initialized && m_rhi == rhi) return;
    release();
    m_rhi = rhi;

    // Ensure we have a body mesh to upload (default shape if setShape was
    // not called yet).
    if (m_shapeVerts.empty()) {
        buildShapeMesh(m_shape, m_shapeVerts);
        m_shapeVertCount = m_shapeVerts.size() / sizeof(V);
    }

    // Shape buffer is Dynamic so setShape can rebuild on the fly.
    const quint32 shapeBytes = static_cast<quint32>(m_shapeVerts.size());
    m_quadVbo.reset(rhi->newBuffer(QRhiBuffer::Dynamic,
                                   QRhiBuffer::VertexBuffer, shapeBytes));
    m_quadVbo->create();
    m_shapeCapacityBytes = shapeBytes;

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
    m_lenVbo.reset(makeDyn(sizeof(float)));
    m_wdtVbo.reset(makeDyn(sizeof(float)));
    m_posCapacityBytes = 8 * sizeof(float);
    m_angCapacityBytes = sizeof(float);
    m_colCapacityBytes = 4;
    m_lenCapacityBytes = sizeof(float);
    m_wdtCapacityBytes = sizeof(float);

    // Uniform buffer: mat4 proj (64) + vec4 minSize (16). std140 = 80 bytes.
    m_ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic,
                                QRhiBuffer::UniformBuffer, 80));
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
        { sizeof(V),           QRhiVertexInputBinding::PerVertex },     // 0: pos + tint
        { 2 * sizeof(float),   QRhiVertexInputBinding::PerInstance },  // 1: pos
        { sizeof(float),       QRhiVertexInputBinding::PerInstance },  // 2: angle
        { 4 * sizeof(quint8),  QRhiVertexInputBinding::PerInstance },  // 3: rgba
        { sizeof(float),       QRhiVertexInputBinding::PerInstance },  // 4: length
        { sizeof(float),       QRhiVertexInputBinding::PerInstance },  // 5: width
    });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2,    0 },
        { 0, 5, QRhiVertexInputAttribute::UNormByte4, 2 * sizeof(float) },  // per-vertex tint
        { 1, 1, QRhiVertexInputAttribute::Float2,    0 },
        { 2, 2, QRhiVertexInputAttribute::Float,     0 },
        { 3, 3, QRhiVertexInputAttribute::UNormByte4, 0 },
        { 4, 4, QRhiVertexInputAttribute::Float,     0 },
        { 5, 6, QRhiVertexInputAttribute::Float,     0 },  // per-instance width
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
        m_lenStaging.clear();
        m_wdtStaging.clear();
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
    m_lenStaging.assign(snap->veh_lengths.begin(), snap->veh_lengths.end());
    if (m_lenStaging.size() < n) m_lenStaging.resize(n, 5.0f);
    m_wdtStaging.assign(snap->veh_widths.begin(), snap->veh_widths.end());
    if (m_wdtStaging.size() < n) m_wdtStaging.resize(n, 1.8f);

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
        const std::size_t needed = m_shapeVerts.size();
        if (needed > m_shapeCapacityBytes) {
            const std::size_t newCap = std::max(needed, m_shapeCapacityBytes * 2);
            m_quadVbo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                             QRhiBuffer::VertexBuffer,
                                             static_cast<quint32>(newCap)));
            m_quadVbo->create();
            m_shapeCapacityBytes = newCap;
        }
        batch->updateDynamicBuffer(m_quadVbo.get(), 0,
                                   static_cast<quint32>(needed),
                                   m_shapeVerts.data());
        m_uploadQuad = false;
    }

    // Upload projection + min-size every frame; cheap (80 B std140 block).
    // Layout: float[16] proj, float[4] minSize.
    {
        float ubo[20];
        std::memcpy(ubo, m_projStaging, 16 * sizeof(float));
        std::memcpy(ubo + 16, m_minSize, 4 * sizeof(float));
        batch->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(ubo), ubo);
    }

    if (!m_uploadDirty || m_instanceCount == 0) return;

    const std::size_t posBytes = m_posStaging.size() * sizeof(float);
    const std::size_t angBytes = m_angStaging.size() * sizeof(float);
    const std::size_t colBytes = m_colStaging.size();
    const std::size_t lenBytes = m_lenStaging.size() * sizeof(float);
    const std::size_t wdtBytes = m_wdtStaging.size() * sizeof(float);

    ensureCapacity(m_rhi, m_posVbo, m_posCapacityBytes, posBytes,
                   QRhiBuffer::VertexBuffer);
    ensureCapacity(m_rhi, m_angVbo, m_angCapacityBytes, angBytes,
                   QRhiBuffer::VertexBuffer);
    ensureCapacity(m_rhi, m_colVbo, m_colCapacityBytes, colBytes,
                   QRhiBuffer::VertexBuffer);
    ensureCapacity(m_rhi, m_lenVbo, m_lenCapacityBytes, lenBytes,
                   QRhiBuffer::VertexBuffer);
    ensureCapacity(m_rhi, m_wdtVbo, m_wdtCapacityBytes, wdtBytes,
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
    batch->updateDynamicBuffer(m_lenVbo.get(), 0,
                               static_cast<quint32>(lenBytes),
                               m_lenStaging.data());
    batch->updateDynamicBuffer(m_wdtVbo.get(), 0,
                               static_cast<quint32>(wdtBytes),
                               m_wdtStaging.data());

    m_uploadDirty = false;
}

void VehicleLayerRhi::setProjection(const float* projectionMatColMajor) {
    std::memcpy(m_projStaging, projectionMatColMajor, 16 * sizeof(float));
}

void VehicleLayerRhi::setMinSize(float minLengthMeters, float minWidthMeters) {
    m_minSize[0] = minLengthMeters;
    m_minSize[1] = minWidthMeters;
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
        { m_lenVbo.get(),  0 },
        { m_wdtVbo.get(),  0 },
    };
    cb->setVertexInput(0, 6, bindings);
    cb->draw(static_cast<quint32>(m_shapeVertCount),
             static_cast<quint32>(m_instanceCount));
}
