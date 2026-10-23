/**
 * @file app/streaming/scripting.h
 * @brief Minimal scripting support for Moonlight streams.
 */
#pragma once

#include <QString>

namespace Scripting {

/**
 * @brief Start interpreting the script pointed to by MOONLIGHT_AHK_SCRIPT.
 *
 * The script is read from the path given by the environment variable, or from
 * the file passed explicitly. Commands are executed on a background thread.
 */
void start(const QString &path = QString());

/**
 * @brief Stop the running script and release any held keys.
 */
void stop();

}  // namespace Scripting
