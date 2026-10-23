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

#include "Limelight.h"

#include <cstdlib>
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
 * @brief Resolve the virtual key code of the key held while a stream is active.
 *
 * @return Virtual key code (VK_*) of the configured key, defaulting to W.
 */
unsigned short hold_key_code() {
  const char *env = std::getenv("MOONLIGHT_HOLD_KEY");
  char key = 'w';
  if (env != nullptr && env[0] != '\0') {
    key = static_cast<char>(std::tolower(static_cast<unsigned char>(env[0])));
  }

  if (key >= 'a' && key <= 'z') {
    return static_cast<unsigned short>(0x41 + (key - 'a'));
  }
  if (key >= '0' && key <= '9') {
    return static_cast<unsigned short>(0x30 + (key - '0'));
  }
  switch (key) {
    case ' ':
      return 0x20;
    case '\r':
    case '\n':
      return 0x0D;
    case '\t':
      return 0x09;
    default:
      return 0x57;  // VK_W
  }
}

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

/**
 * @brief Detect the codec of a raw Annex-B elementary stream.
 *
 * @param path Path of the raw recorded bitstream.
 * @return "hevc", "h264", or an empty string when the codec could not be detected.
 */
QString detect_codec(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray data = file.read(4096);
    file.close();

    for (int i = 0; i + 3 < data.size(); ++i) {
        int nal = 0;
        if (static_cast<unsigned char>(data[i]) == 0 && static_cast<unsigned char>(data[i + 1]) == 0 &&
            static_cast<unsigned char>(data[i + 2]) == 1) {
            nal = i + 3;
        } else if (i + 4 < data.size() && static_cast<unsigned char>(data[i]) == 0 &&
                   static_cast<unsigned char>(data[i + 1]) == 0 && static_cast<unsigned char>(data[i + 2]) == 0 &&
                   static_cast<unsigned char>(data[i + 3]) == 1) {
            nal = i + 4;
        } else {
            continue;
        }
        if (nal >= data.size()) {
            break;
        }

        const unsigned char header = static_cast<unsigned char>(data[nal]);
        // H.264 NAL types are the low 5 bits, HEVC NAL types are bits 1-6.
        const unsigned char h264Type = header & 0x1F;
        const unsigned char hevcType = (header >> 1) & 0x3F;

        if (h264Type == 7 || h264Type == 8) {
            return "h264";
        }
        if (hevcType == 32 || hevcType == 33 || hevcType == 34) {
            return "hevc";
        }
    }

    return QString();
}

/**
 * @brief Resolve the path of the stream-connected flag file.
 *
 * @return Absolute path of the flag file.
 */
QString stream_flag_path()
{
    const char *env = std::getenv("MOONLIGHT_STREAM_FLAG");
    if (env != nullptr && env[0] != '\0') {
        return QString::fromUtf8(env);
    }
    return QCoreApplication::applicationDirPath() + "/output/stream_connected.flag";
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
    // Store the codec so a recording left behind by a terminated session can be muxed.
    QFile codecFile(target + ".codec");
    if (codecFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
      codecFile.write(is_hevc(videoFormat) ? "hevc" : "h264");
      codecFile.close();
    }
    // Signal external tools (e.g. the AHK plugin) that a stream has connected.
    QFile flagFile(stream_flag_path());
    if (flagFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      flagFile.close();
    }
    // Hold W down on the remote host while the stream is active.
    LiSendKeyboardEvent2(hold_key_code(), KEY_ACTION_DOWN, 0, 0);
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
  // Release W on the remote host.
      LiSendKeyboardEvent2(hold_key_code(), KEY_ACTION_UP, 0, 0);

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

  // Remove the connection flag so external tools know the stream ended.
  QFile(stream_flag_path()).remove();
  }

  if (recordPath.empty() || !started) {
    // Even if we didn't record, release W if it was held.
      LiSendKeyboardEvent2(hold_key_code(), KEY_ACTION_UP, 0, 0);
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

void VideoRecorder::muxPending()
{
    // A stale flag from a previous run should never be treated as "connected".
    QFile(stream_flag_path()).remove();

    QString raw = qgetenv("MOONLIGHT_RECORD_PATH");
  if (raw.isEmpty()) {
    QDir outputDir(QCoreApplication::applicationDirPath() + "/output");
    raw = outputDir.filePath("client_received.h264");
  }

  QFileInfo info(raw);
  if (!info.exists() || info.size() == 0) {
    return;
  }

  const QString ffmpeg = find_ffmpeg();
  if (ffmpeg.isEmpty()) {
    return;
  }

  QString mp4 = raw;
  if (mp4.endsWith(".h264")) {
    mp4.chop(5);
  }
  mp4 += ".mp4";

  // Detect the codec from the bitstream so the right demuxer is used.
  QStringList demuxers;
  const QString codec = detect_codec(raw);
  if (codec == "hevc") {
    demuxers << "hevc";
  } else if (codec == "h264") {
    demuxers << "h264";
  } else {
    demuxers << "h264" << "hevc";
  }

  for (const QString &demuxer : demuxers) {
    QStringList args;
    args << "-y" << "-f" << demuxer << "-i" << raw << "-c" << "copy" << mp4;
    if (QProcess::startDetached(ffmpeg, args)) {
      SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Muxing pending client recording to %s", mp4.toUtf8().constData());
      return;
    }
  }
}
