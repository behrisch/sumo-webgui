#include "NetworkOverlayWidget.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QLinearGradient>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <algorithm>
#include <cmath>

#include "NetworkView.h"
#include "sim/NetworkGeometry.h"

NetworkOverlayWidget::NetworkOverlayWidget(NetworkView* view)
    : QWidget(view), m_view(view) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
}

void NetworkOverlayWidget::drawScaleBar(QPainter& p) {
    const NetworkView* v = m_view;
    const double ppu = v->m_cam.pixelsPerUnit();
    if (!(ppu > 0.0)) return;
    const double targetPx = 100.0;
    double meters = targetPx / ppu;
    const double mag = std::pow(10.0, std::floor(std::log10(meters)));
    const double mantissa = meters / mag;
    double nice;
    if      (mantissa < 1.5) nice = 1.0;
    else if (mantissa < 3.5) nice = 2.0;
    else if (mantissa < 7.5) nice = 5.0;
    else                     nice = 10.0;
    meters = nice * mag;
    const double widthPx = meters * ppu / v->devicePixelRatioF();

    QString label;
    if (meters >= 1000.0) label = QString::number(meters / 1000.0, 'g', 3) + " km";
    else if (meters >= 1.0) label = QString::number(meters, 'g', 3) + " m";
    else label = QString::number(meters * 100.0, 'g', 3) + " cm";

    const int margin = 12;
    const QFontMetrics fm(p.font());
    const int barH = 6;
    const int textH = fm.height();
    const int boxW = static_cast<int>(widthPx) + 2 * margin;
    const int boxH = barH + textH + 8;
    const QRectF box(v->width() - boxW - margin, v->height() - boxH - margin, boxW, boxH);
    p.fillRect(box, QColor(0, 0, 0, 160));
    p.setPen(Qt::white);
    const int x0 = static_cast<int>(box.left()) + margin;
    const int y0 = static_cast<int>(box.bottom()) - 4 - barH;
    p.drawLine(x0, y0, x0 + static_cast<int>(widthPx), y0);
    p.drawLine(x0, y0 - 3, x0, y0 + 3);
    p.drawLine(x0 + static_cast<int>(widthPx), y0 - 3,
               x0 + static_cast<int>(widthPx), y0 + 3);
    p.drawText(static_cast<int>(box.left()) + margin,
               static_cast<int>(box.top()) + fm.ascent() + 2, label);
}

void NetworkOverlayWidget::drawLegend(QPainter& p) {
    const NetworkView* v = m_view;
    if (!v->m_snap || v->m_snap->lane_attr_label.empty()) return;
    const QString label = QString::fromStdString(v->m_snap->lane_attr_label);
    const QFontMetrics fm(p.font());
    const int margin = 12;
    const int barW = 200, barH = 10;
    const int textW = fm.horizontalAdvance(label);
    const int boxW = std::max(barW, textW) + 2 * margin;
    // Title + gradient + tick labels under the bar.
    const QString lo = QStringLiteral("low");
    const QString hi = QStringLiteral("high");
    const int boxH = fm.height() + 4 + barH + 2 + fm.height() + 8;
    const QRectF box(margin, v->height() - boxH - margin, boxW, boxH);
    p.fillRect(box, QColor(0, 0, 0, 160));
    p.setPen(Qt::white);
    p.drawText(static_cast<int>(box.left()) + margin,
               static_cast<int>(box.top()) + fm.ascent() + 2, label);
    QLinearGradient g(box.left() + margin, 0, box.left() + margin + barW, 0);
    g.setColorAt(0.0, QColor(255, 0,   0));
    g.setColorAt(0.5, QColor(255, 255, 0));
    g.setColorAt(1.0, QColor(  0, 255, 0));
    const QRectF bar(box.left() + margin,
                     box.top() + fm.height() + 4, barW, barH);
    p.fillRect(bar, g);
    p.setPen(QColor(220, 220, 220));
    // Tick labels under the gradient ends.
    const int tickY = static_cast<int>(bar.bottom()) + fm.ascent() + 2;
    p.drawText(static_cast<int>(bar.left()), tickY, lo);
    p.drawText(static_cast<int>(bar.right()) - fm.horizontalAdvance(hi),
               tickY, hi);
}

void NetworkOverlayWidget::drawInfoBox(QPainter& p) {
    const NetworkView* v = m_view;
    if (v->m_picked.kind == NetworkView::PickKind::None) return;
    const QFontMetrics fm(p.font());
    int maxW = fm.horizontalAdvance(v->m_picked.title);
    for (const auto& l : v->m_picked.lines)
        maxW = std::max(maxW, fm.horizontalAdvance(l));
    const int margin = 10;
    const int lineH = fm.height();
    const int boxW = maxW + 2 * margin;
    const int boxH = lineH * (1 + static_cast<int>(v->m_picked.lines.size())) + 2 * margin;
    const QRectF box(margin, 12 + margin, boxW, boxH);
    p.fillRect(box, QColor(0, 0, 0, 200));
    p.setPen(QColor(255, 200, 80));
    p.drawText(static_cast<int>(box.left()) + margin,
               static_cast<int>(box.top()) + margin + fm.ascent(),
               v->m_picked.title);
    p.setPen(Qt::white);
    int y = static_cast<int>(box.top()) + margin + fm.ascent() + lineH;
    for (const auto& l : v->m_picked.lines) {
        p.drawText(static_cast<int>(box.left()) + margin, y, l);
        y += lineH;
    }
}

void NetworkOverlayWidget::paintEvent(QPaintEvent*) {
    if (!m_view) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawLegend(p);
    drawScaleBar(p);
    drawInfoBox(p);
}
