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

// QRhi reimplementation of the vehicle layer. Renders one instanced quad per
// vehicle with per-instance position, navi-angle and rgba colour. Works on
// any QRhi backend (Vulkan / Metal / D3D12 / OpenGL) — shaders are baked via
// qsb at build time.
//
// NOTE: QRhi has no `Double2` vertex-attribute format (none of the modern
// native APIs do). VehicleLayerRhi therefore narrows the f64 position column
// from `SimSnapshot::veh_positions` to a cached f32 buffer in setSnapshot().
// For SUMO scenarios with coordinates < ~1e6 m this preserves ~mm precision;
// scenarios with very large projected origins should subtract a fixed offset
// upstream (TODO once the migration touches geo-referenced rendering).
class VehicleLayerRhi {
public:
    // Mesh selector for the instanced body geometry. Mirrors the
    // `VehicleShape` registry in ecal_deck/frontend/src/layers/vehicleShapes.ts.
    //   Rectangle: simple axis-aligned body (legacy default).
    //   Triangle:  isoceles, point forward (anchor at the tip).
    //   Car:       realistic car silhouette: body polygon + darker front
    //              bumper overlay + black windshield strip, blended via a
    //              per-vertex tint multiplier (255=body color, 128=darker,
    //              0=black). Matches the deck.gl SimpleMeshLayer geometry.
    //   Circle:    radial fan, useful for low-zoom dots / non-car classes.
    enum class Shape { Rectangle, Triangle, Car, Circle };

    VehicleLayerRhi();
    ~VehicleLayerRhi();

    VehicleLayerRhi(const VehicleLayerRhi&)            = delete;
    VehicleLayerRhi& operator=(const VehicleLayerRhi&) = delete;

    // Build the pipeline against the given QRhi + render pass. Safe to call
    // again after a release().
    void initialize(QRhi* rhi,
                    QRhiRenderPassDescriptor* rp,
                    int sampleCount = 1);

    // Tear down all GPU resources (e.g. on swapchain rebuild).
    void release();

    // Stash a new snapshot. CPU-side data is converted/staged into pending
    // buffers; uploads happen in resourceUpdate().
    void setSnapshot(const SimSnapshotPtr& snap);

    // Append GPU uploads for the latest snapshot to `batch`. Call between
    // QRhiCommandBuffer::beginPass and the draw, or before beginPass.
    void resourceUpdate(QRhiResourceUpdateBatch* batch);

    // Stash the projection. Uploaded to the uniform buffer in the next
    // resourceUpdate() call. Call this before resourceUpdate().
    void setProjection(const float* projectionMatColMajor);

    // Swap the per-vehicle body mesh. Cheap (rebuilds a small static VBO on
    // the next resourceUpdate). Default is Rectangle.
    void setShape(Shape s);

    // Record the draw into `cb`. Caller has already started a render pass.
    void render(QRhiCommandBuffer* cb);

    std::size_t instanceCount() const { return m_instanceCount; }

private:
    QRhi* m_rhi = nullptr;

    std::unique_ptr<QRhiBuffer>                 m_quadVbo;
    std::unique_ptr<QRhiBuffer>                 m_posVbo;
    std::unique_ptr<QRhiBuffer>                 m_angVbo;
    std::unique_ptr<QRhiBuffer>                 m_colVbo;
    std::unique_ptr<QRhiBuffer>                 m_lenVbo;
    std::unique_ptr<QRhiBuffer>                 m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pipeline;

    // CPU staging for the next upload. We hold them so the QRhiResourceUpdate
    // batch can reference stable memory until commit.
    std::vector<float>     m_posStaging;  // (x,y) per vehicle, narrowed from f64
    std::vector<float>     m_angStaging;  // copy of navi-degrees
    std::vector<uint8_t>   m_colStaging;  // copy of rgba8
    std::vector<float>     m_lenStaging;  // copy of per-vehicle length (m)
    float                  m_projStaging[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    std::size_t m_instanceCount    = 0;
    std::size_t m_posCapacityBytes = 0;  // current size of m_posVbo in bytes
    std::size_t m_angCapacityBytes = 0;
    std::size_t m_colCapacityBytes = 0;
    std::size_t m_lenCapacityBytes = 0;

    bool m_uploadQuad   = true;  // one-time / on shape change static upload
    bool m_uploadDirty  = false; // new snapshot waiting
    bool m_initialized  = false;

    // Active shape + the CPU mesh data (regenerated on setShape).
    Shape                m_shape = Shape::Rectangle;
    // Interleaved (pos.xy, tint, 3 pad) — 12 B per vertex. Defined in the
    // .cpp; we use std::vector<std::byte> here to keep the header free of
    // the private V struct.
    std::vector<std::byte> m_shapeVerts;
    std::size_t          m_shapeVertCount = 0;
    std::size_t          m_shapeCapacityBytes = 0;
};
