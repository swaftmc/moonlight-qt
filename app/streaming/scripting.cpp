/**
 * @file app/streaming/scripting.cpp
 * @brief Minimal scripting support for Moonlight streams.
 */
#include "scripting.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <SDL.h>

extern "C" {
#include <Limelight.h>
}

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop {false};
std::thread g_thread;
std::mutex g_held_mutex;
std::vector<unsigned short> g_held_keys;

/**
 * @brief Convert a key name to a Windows virtual key code.
 *
 * @param name Key name (a-z, 0-9, space, enter, tab, esc, ...).
 * @return Virtual key code, or 0 when the name is unknown.
 */
unsigned short key_code(const std::string &name) {
  std::string lower = name;
  for (char &c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  if (lower.size() == 1) {
    char c = lower[0];
    if (c >= 'a' && c <= 'z') {
      return static_cast<unsigned short>(0x41 + (c - 'a'));
    }
    if (c >= '0' && c <= '9') {
      return static_cast<unsigned short>(0x30 + (c - '0'));
    }
  }

  if (lower == "space") return 0x20;
  if (lower == "enter" || lower == "return") return 0x0D;
  if (lower == "tab") return 0x09;
  if (lower == "esc" || lower == "escape") return 0x1B;
  if (lower == "backspace") return 0x08;
  if (lower == "shift") return 0x10;
  if (lower == "ctrl" || lower == "control") return 0x11;
  if (lower == "alt") return 0x12;
  if (lower == "up") return 0x26;
  if (lower == "down") return 0x28;
  if (lower == "left") return 0x25;
  if (lower == "right") return 0x27;

  return 0;
}

/**
 * @brief Hold a key down, recording it so it can be released later.
 *
 * @param code Virtual key code to hold.
 */
void hold_key(unsigned short code) {
  if (code == 0) {
    return;
  }
  LiSendKeyboardEvent2(code, KEY_ACTION_DOWN, 0, 0);
  std::lock_guard<std::mutex> locker(g_held_mutex);
  g_held_keys.push_back(code);
}

/**
 * @brief Release a held key.
 *
 * @param code Virtual key code to release.
 */
void release_key(unsigned short code) {
  if (code == 0) {
    return;
  }
  LiSendKeyboardEvent2(code, KEY_ACTION_UP, 0, 0);
  std::lock_guard<std::mutex> locker(g_held_mutex);
  g_held_keys.erase(std::remove(g_held_keys.begin(), g_held_keys.end(), code), g_held_keys.end());
}

/**
 * @brief Release every key currently held by the script.
 */
void release_all() {
  std::vector<unsigned short> held;
  {
    std::lock_guard<std::mutex> locker(g_held_mutex);
    held = g_held_keys;
    g_held_keys.clear();
  }
  for (unsigned short code : held) {
    LiSendKeyboardEvent2(code, KEY_ACTION_UP, 0, 0);
  }
}

/**
 * @brief Execute a single script command.
 *
 * @param command Command line to execute.
 */
void execute(const std::string &command) {
  std::istringstream stream(command);
  std::string verb;
  stream >> verb;
  for (char &c : verb) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  if (verb.empty() || verb[0] == '#' || verb[0] == ';') {
    return;
  }

  if (verb == "hold") {
    std::string key;
    stream >> key;
    hold_key(key_code(key));
  } else if (verb == "release") {
    std::string key;
    stream >> key;
    release_key(key_code(key));
  } else if (verb == "press") {
    std::string key;
    stream >> key;
    const unsigned short code = key_code(key);
    if (code != 0) {
      LiSendKeyboardEvent2(code, KEY_ACTION_DOWN, 0, 0);
      LiSendKeyboardEvent2(code, KEY_ACTION_UP, 0, 0);
    }
  } else if (verb == "sleep") {
    int ms = 0;
    stream >> ms;
    if (ms < 0) {
      ms = 0;
    }
    const int step = 50;
    int waited = 0;
    while (waited < ms && !g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(step));
      waited += step;
    }
  }
}

/**
 * @brief Background thread that runs the script.
 *
 * @param path Path of the script file.
 */
void run(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not open script file: %s", path.toUtf8().constData());
    return;
  }

  const QString contents = QString::fromUtf8(file.readAll());
  file.close();

  std::istringstream stream(contents.toStdString());
  std::string line;
  while (!g_stop.load() && std::getline(stream, line)) {
    // Strip trailing carriage return.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    execute(line);
  }

  release_all();
}

}  // namespace

void Scripting::start(const QString &path) {
  QString scriptPath = path;
  if (scriptPath.isEmpty()) {
    scriptPath = qgetenv("MOONLIGHT_AHK_SCRIPT");
  }
  if (scriptPath.isEmpty()) {
    return;
  }

  g_stop.store(false);
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_thread = std::thread(run, scriptPath);
}

void Scripting::stop() {
  g_stop.store(true);
  if (g_thread.joinable()) {
    g_thread.join();
  }
  release_all();
}
