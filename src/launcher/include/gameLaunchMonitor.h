#pragma once

#include <QString>
#include <QStringList>

class Configuration;
class QProcess;
class QWidget;

// Owns the output reader and startup dialog until this process finishes.
void StartGameWithPreparation(QProcess* process, const QString& interpreter,
                              const QStringList& arguments, const Configuration& config,
                              QWidget* parent);
