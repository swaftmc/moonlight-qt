/**
 * @file app/streaming/videorecorder.cpp
 * @brief Recording of the encoded video bitstream received from the host.
 */
#include "videorecorder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <SDL.h>

#include <fstream>
#include <mutex>
#include <string>

namespace {

std::mutex g_record_mutex;
std::ofstream g_record_file;
std::string g_record_path;
int g_video_format = 0;
bool g_recording = false;
bool g_started = false;

/**
 * @brief Check whether the negotiated video format is an HEVC (H.265) stream.
 *
 * @param video_format Negotiated video format.
 * @return True for HEVC formats.
 */
bool is_hevc(int video_format) {
  return (video_format & 0x0100) != 0;
}

/**
 * @brief Locate the ffmpeg executable used to mux the recording.
 *
 * @return Path of the ffmpeg executable, or an empty string when none was found.
 */
QString find_ffmpeg() {
  QStringList candidates;
  candidates << QCoreApplication::applicationDirPath() + "/ffmpeg.exe"
             << QCoreApplication::applicationDirPath() + "/ffmpeg/ffmpeg.exe"
             << "C:/msys64/ucrt64/bin/ffmpeg.exe";
  for (const QString &candidate : candidates) {
    if (QFileInfo::exists(candidate)) {
      return candidate;
    }
  }
  return QString();
}

}  // namespace

bool VideoRecorder::start(const QString &path, int videoFormat)
{
  std::lock_guard<std::mutex> locker(g_record_mutex);

  if (g_record_file.is_open()) {
    g_record_file.close();
  }

  QString target = path;
  if (target.isEmpty()) {
    QDir outputDir(QCoreApplication::applicationDirPath() + "/output");
    outputDir.mkpath(".");
    target = outputDir.filePath("client_received.h264");
  } else {
    QDir(QFileInfo(target).absolutePath()).mkpath(".");
  }

  g_video_format = videoFormat;
  g_record_path = target.toStdString();
  g_record_file.open(g_record_path, std::ios::binary | std::ios::trunc);
  g_recording = g_record_file.is_open();
  g_started = false;

  if (g_recording) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Recording client video to %s", target.toUtf8().constData());
  } else {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to open client recording file: %s", target.toUtf8().constData());
  }

  return g_recording;
}

void VideoRecorder::write(PDECODE_UNIT du)
{
  if (du == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> locker(g_record_mutex);
  if (!g_recording || !g_record_file.is_open()) {
    return;
  }

  // Wait for the first key frame so the elementary stream starts with the
  // codec configuration data (SPS/PPS/VPS) required to mux the recording.
  if (!g_started) {
    if (du->frameType != FRAME_TYPE_IDR) {
      return;
    }
    g_started = true;
  }

  for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
    if (entry->data != nullptr && entry->length > 0) {
      g_record_file.write(reinterpret_cast<const char *>(entry->data), entry->length);
    }
  }
  g_record_file.flush();
}

void VideoRecorder::stop()
{
  std::string recordPath;
  int videoFormat = 0;
  bool started = false;
  {
    std::lock_guard<std::mutex> locker(g_record_mutex);
    if (g_record_file.is_open()) {
      g_record_file.close();
    }
    g_recording = false;
    recordPath = g_record_path;
    videoFormat = g_video_format;
    started = g_started;
    g_started = false;
    g_record_path.clear();
  }

  if (recordPath.empty() || !started) {
    return;
  }

  QString raw = QString::fromStdString(recordPath);
  QString mp4 = raw;
  if (mp4.endsWith(".h264")) {
    mp4.chop(5);
  }
  mp4 += ".mp4";

  const QString ffmpeg = find_ffmpeg();
  if (ffmpeg.isEmpty()) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ffmpeg was not found, keeping the raw recording at %s", raw.toUtf8().constData());
    return;
  }

  QStringList args;
  args << "-y" << "-f" << (is_hevc(videoFormat) ? "hevc" : "h264") << "-i" << raw << "-c" << "copy" << mp4;
  if (QProcess::startDetached(ffmpeg, args)) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Muxing client recording to %s", mp4.toUtf8().constData());
  } else {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to start ffmpeg to mux the client recording");
  }
}

bool VideoRecorder::isActive()
{
  std::lock_guard<std::mutex> locker(g_record_mutex);
  return g_recording;
}
