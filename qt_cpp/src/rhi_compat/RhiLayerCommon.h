#pragma once

// Shared utilities for QRhi-based layers: shader loading from the qt6_add_shaders
// resource prefix, dynamic-buffer growth, and the standard premultiplied-alpha
// blend state used by every transparent layer in the network view.

#include <algorithm>
#include <cstddef>
#include <memory>

#include <QtCore/QFile>
#include <QtCore/QString>

#include "rhi_compat/rhi_compat.h"

namespace rhi_compat {

inline QShader loadShader(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("rhi_compat::loadShader: failed to open %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

// Grow a Dynamic VertexBuffer-like resource to at least `wanted` bytes (with
// 2x slack to dampen per-frame rebuilds). Returns true if the buffer was
// rebuilt — callers don't need to rebind anything for plain vertex buffers,
// but for buffers referenced by a ShaderResourceBindings (uniforms etc.) the
// SRB must be rebuilt.
inline bool ensureCapacity(QRhi* rhi,
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

// Standard alpha-blending target state for our 2D layers. All RHI layers
// that emit alpha should call this on their TargetBlend before pushing it
// into the pipeline.
inline QRhiGraphicsPipeline::TargetBlend alphaBlend() {
    QRhiGraphicsPipeline::TargetBlend t;
    t.enable      = true;
    t.srcColor    = QRhiGraphicsPipeline::SrcAlpha;
    t.dstColor    = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    t.srcAlpha    = QRhiGraphicsPipeline::One;
    t.dstAlpha    = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return t;
}

}  // namespace rhi_compat
