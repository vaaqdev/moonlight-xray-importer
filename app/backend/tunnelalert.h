#pragma once

#include <QString>

// Shows a blocking critical alert. Implemented with AppKit because the app
// does not link QtWidgets.
void showTunnelStartupError(const QString& message);
