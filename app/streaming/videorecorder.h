/**
 * @file app/streaming/videorecorder.h
 * @brief Recording of the encoded video bitstream received from the host.
 */
#pragma once

#include <QString>

#include "Limelight.h"

namespace VideoRecorder {

/**
 * @brief Start recording the received encoded video to a file.
 *
 * @param path Destination file path. An empty path uses `output/client_received.h264`
 *             relative to the application directory.
 * @param videoFormat Negotiated video format of the stream. Used to pick the
 *                    correct elementary stream demuxer when muxing to MP4.
 * @return True when the file was opened successfully.
 */
bool start(const QString &path = QString(), int videoFormat = 0);

/**
 * @brief Append an encoded decode unit to the recording.
 *
 * @param du Decode unit received from the host. May be null.
 */
void write(PDECODE_UNIT du);

/**
 * @brief Stop recording and mux the captured bitstream into an MP4 file.
 */
void stop();

/**
 * @brief Check whether a recording is currently active.
 *
 * @return True when data is being written to the recording file.
 */
bool isActive();

/**
 * @brief Mux a recording left behind by a previous run into an MP4 file.
 */
void muxPending();

}  // namespace VideoRecorder
