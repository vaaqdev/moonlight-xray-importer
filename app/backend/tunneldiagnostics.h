#pragma once
#include <QJsonObject>
#include <QString>

// Only sanitized diagnostic text is persisted; never save profiles or credentials.
namespace TunnelDiagnostics {
void protectProfile(const QJsonObject& profile);
QString sanitize(const QString& text);
void write(const QString& stage, const QString& text);
QString directory();
QString exportReport(const QString& status, const QString& error);
}
