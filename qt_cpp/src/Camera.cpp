#include "Camera.h"

#include <algorithm>
#include <cmath>

void Camera::setViewport(int wPx, int hPx) noexcept {
    m_wPx = std::max(1, wPx);
    m_hPx = std::max(1, hPx);
}

void Camera::setPixelsPerUnit(double ppu) noexcept {
    constexpr double kMin = 1e-6;
    constexpr double kMax = 1e7;
    m_ppu = std::clamp(ppu, kMin, kMax);
}

void Camera::zoomAtPixel(double pxX, double pxY, double factor) noexcept {
    double wx, wy;
    pixelToWorld(pxX, pxY, wx, wy);
    setPixelsPerUnit(m_ppu * factor);
    // Re-anchor so (pxX,pxY) still maps to (wx,wy).
    double wx2, wy2;
    pixelToWorld(pxX, pxY, wx2, wy2);
    m_cx += wx - wx2;
    m_cy += wy - wy2;
}

void Camera::panPixels(double dxPx, double dyPx) noexcept {
    m_cx -= dxPx / m_ppu;
    m_cy += dyPx / m_ppu;  // pixel y grows downward; world y grows upward.
}

void Camera::fitBounds(double minX, double minY,
                       double maxX, double maxY,
                       double padding) noexcept {
    const double w = std::max(1e-6, maxX - minX);
    const double h = std::max(1e-6, maxY - minY);
    const double pad = 1.0 + std::max(0.0, padding) * 2.0;
    const double ppuX = m_wPx / (w * pad);
    const double ppuY = m_hPx / (h * pad);
    setPixelsPerUnit(std::min(ppuX, ppuY));
    m_cx = 0.5 * (minX + maxX);
    m_cy = 0.5 * (minY + maxY);
}

void Camera::pixelToWorld(double pxX, double pxY,
                          double& outX, double& outY) const noexcept {
    const double dx = pxX - 0.5 * m_wPx;
    const double dy = pxY - 0.5 * m_hPx;
    outX = m_cx + dx / m_ppu;
    outY = m_cy - dy / m_ppu;
}

std::array<float, 16> Camera::projection() const noexcept {
    // World -> NDC. half-extents in world units:
    const double hx = 0.5 * m_wPx / m_ppu;
    const double hy = 0.5 * m_hPx / m_ppu;

    const float sx = static_cast<float>(1.0 / hx);
    const float sy = static_cast<float>(1.0 / hy);
    const float tx = static_cast<float>(-m_cx / hx);
    const float ty = static_cast<float>(-m_cy / hy);

    // Column-major 4x4: x' = sx*x + tx, y' = sy*y + ty, z'=0, w'=1.
    std::array<float, 16> m{};
    m[0]  = sx;  m[5]  = sy;  m[10] = 1.0f; m[15] = 1.0f;
    m[12] = tx;  m[13] = ty;
    return m;
}
