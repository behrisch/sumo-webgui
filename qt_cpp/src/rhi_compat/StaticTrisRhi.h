#pragma once

#include <cstring>
#include <memory>
#include <vector>

#include <QtCore/qglobal.h>
#include "rhi_compat/RhiLayerCommon.h"
#include "rhi_compat/TrisPasses.h"

// Generic "static buffer of TrisColorVertex" container. Holds CPU staging
// + a Dynamic vertex buffer that grows on demand. setVertices() stages,
// resourceUpdate() commits, render() issues one draw via the supplied pass.
//
// Use one instance per logical geometry chunk. NetworkView owns a handful
// of these for the various static layers (detector, stop line, ped areas,
// rails, polygons, network etc.) — they all share the same pass pipelines.
class StaticTrisRhi {
public:
    void release() {
        m_vbo.reset();
        m_capacityBytes = 0;
        m_vertexCount   = 0;
        m_rhi           = nullptr;
    }

    void initialize(QRhi* rhi) {
        if (m_rhi == rhi && m_vbo) return;
        release();
        m_rhi = rhi;
        m_vbo.reset(rhi->newBuffer(QRhiBuffer::Dynamic,
                                   QRhiBuffer::VertexBuffer, 64));
        m_vbo->create();
        m_capacityBytes = 64;
    }

    // Move-set the staged vertices.
    void setVertices(std::vector<TrisColorVertex>&& verts) {
        m_staging = std::move(verts);
        m_dirty   = true;
    }

    // In-place access for partial mutation (e.g. EdgeColor per-frame recolor).
    // Call markDirty() afterwards.
    std::vector<TrisColorVertex>& mutableVertices() { return m_staging; }
    void markDirty() { m_dirty = true; }

    void resourceUpdate(QRhiResourceUpdateBatch* batch) {
        if (!m_rhi) return;
        if (!m_dirty) return;
        m_vertexCount = static_cast<quint32>(m_staging.size());
        if (m_vertexCount == 0) { m_dirty = false; return; }
        const std::size_t bytes = m_staging.size() * sizeof(TrisColorVertex);
        rhi_compat::ensureCapacity(m_rhi, m_vbo, m_capacityBytes, bytes,
                                   QRhiBuffer::VertexBuffer);
        batch->updateDynamicBuffer(m_vbo.get(), 0,
                                   static_cast<quint32>(bytes),
                                   m_staging.data());
        m_dirty = false;
    }

    void render(QRhiCommandBuffer* cb, const TrisColorInterleavedPass& pass) const {
        if (m_vertexCount == 0) return;
        pass.drawVB(cb, m_vbo.get(), m_vertexCount);
    }

    quint32 vertexCount() const { return m_vertexCount; }

private:
    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer>  m_vbo;
    std::vector<TrisColorVertex> m_staging;
    std::size_t m_capacityBytes = 0;
    quint32     m_vertexCount   = 0;
    bool        m_dirty         = false;
};
