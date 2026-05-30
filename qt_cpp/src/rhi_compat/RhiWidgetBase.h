#pragma once

// Resolve to QRhiWidget on Qt 6.7+ or to the in-tree RhiHostWidget on
// Qt 6.4 / 6.5 / 6.6. Both expose the same protected virtuals
// (initialize(cb), render(cb), releaseResources) and the same accessors
// (rhi(), renderTarget(), update()), so subclasses can target one base
// alias.

#include <QtCore/qglobal.h>

#if defined(RHI_COMPAT_FORCE_FALLBACK)
   // Build the in-tree RhiHostWidget path even on Qt 6.7+, so we can keep
   // the fallback compiling without dual-installing Qt.
#  include "rhi_compat/RhiHostWidget.h"
namespace rhi_compat {
using RhiWidgetBase = RhiHostWidget;
inline void selectOpenGL(RhiHostWidget* w) { w->setApi(RhiHostWidget::Api::OpenGL); }
}  // namespace rhi_compat
#elif QT_VERSION >= QT_VERSION_CHECK(6, 7, 0) && __has_include(<QtWidgets/QRhiWidget>)
#  include <QtWidgets/QRhiWidget>
namespace rhi_compat {
using RhiWidgetBase = QRhiWidget;
inline void selectOpenGL(QRhiWidget* w) { w->setApi(QRhiWidget::Api::OpenGL); }
}  // namespace rhi_compat
#else
#  include "rhi_compat/RhiHostWidget.h"
namespace rhi_compat {
using RhiWidgetBase = RhiHostWidget;
inline void selectOpenGL(RhiHostWidget* w) { w->setApi(RhiHostWidget::Api::OpenGL); }
}  // namespace rhi_compat
#endif
