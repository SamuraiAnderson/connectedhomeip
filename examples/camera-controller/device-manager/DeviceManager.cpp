/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "DeviceManager.h"

#include <app/data-model/Nullable.h>
#include <commands/interactive/InteractiveCommands.h>
#include <crypto/RandUtils.h>
#include <lib/support/StringBuilder.h>
#include <webrtc-manager/WebRTCManager.h>

#include <chrono>
#include <cstring>
#include <errno.h>
#include <map>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace chip;
using StreamUsageEnum = chip::app::Clusters::Globals::StreamUsageEnum;

namespace camera {

namespace {

constexpr EndpointId kCameraEndpointId = 1;

} // namespace

void DeviceManager::VideoStreamSignalHandler(int sig)
{
    if (sig == SIGCHLD)
    {
        // Reap any terminated child processes
        pid_t pid;
        int status;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        {
            ChipLogProgress(Camera, "Video stream process (PID: %d) terminated", pid);
        }
    }
}

CHIP_ERROR DeviceManager::Init(Controller::DeviceCommissioner * commissioner)
{
    VerifyOrReturnError(commissioner != nullptr, CHIP_ERROR_INCORRECT_STATE);
    mCommissioner = commissioner;
    mAVStreamManagment.Init(commissioner);

    // Register callback for WebRTC session establishment
    WebRTCManager::Instance().SetSessionEstablishedCallback(
        [this](uint16_t streamId) { this->OnWebRTCSessionEstablished(streamId); });

    return CHIP_NO_ERROR;
}

void DeviceManager::Shutdown()
{
    // Stop all video stream processes
    for (auto & pair : mVideoStreamProcesses)
    {
        StopVideoStreamProcess(pair.first);
    }
    mVideoStreamProcesses.clear();

    // Stop all audio stream processes
    for (auto & pair : mAudioStreamProcesses)
    {
        StopAudioStreamProcess(pair.first);
    }
    mAudioStreamProcesses.clear();

    mActiveLiveViewByNode.clear();
    mActiveLiveViewAudioByNode.clear();

    // Disconnect WebRTC session
    WebRTCManager::Instance().Disconnect();
}

CHIP_ERROR DeviceManager::AllocateVideoStream(NodeId nodeId, uint8_t streamUsage, WebRTCOfferType offerType,
                                              Optional<uint16_t> minWidth, Optional<uint16_t> minHeight,
                                              Optional<uint16_t> minFrameRate, Optional<uint32_t> minBitRate)
{
    ChipLogProgress(Camera, "Allocate a video stream on the camera device.");

    CHIP_ERROR error = mAVStreamManagment.AllocateVideoStream(nodeId, kCameraEndpointId, streamUsage, minWidth, minHeight,
                                                              minFrameRate, minBitRate);

    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Camera,
                     "Failed to send VideoStreamAllocate command to the camera device (NodeId: " ChipLogFormatX64
                     "). Error: %" CHIP_ERROR_FORMAT,
                     ChipLogValueX64(nodeId), error.Format());
    }
    else
    {
        mNodeId      = nodeId;
        mStreamUsage = streamUsage;
        mOfferType   = offerType;
    }

    return error;
}

CHIP_ERROR DeviceManager::AllocateLiveViewStream(NodeId nodeId, uint8_t streamUsage, WebRTCOfferType offerType,
                                                 Optional<uint16_t> minWidth, Optional<uint16_t> minHeight,
                                                 Optional<uint16_t> minFrameRate, Optional<uint32_t> minBitRate)
{
    ChipLogProgress(Camera, "Allocate a LiveView (audio + video) stream on the camera device.");

    // Remember the session context and defer the video parameters; the
    // VideoStreamAllocate command is only sent after the AudioStreamAllocate
    // response arrives (see HandleAudioStreamAllocateResponse).
    mNodeId      = nodeId;
    mStreamUsage = streamUsage;
    mOfferType   = offerType;

    mPendingMinResWidth   = minWidth;
    mPendingMinResHeight  = minHeight;
    mPendingMinFrameRate  = minFrameRate;
    mPendingMinBitRate    = minBitRate;
    mPendingAudioStreamId = std::nullopt;

    CHIP_ERROR error = mAVStreamManagment.AllocateAudioStream(nodeId, kCameraEndpointId, streamUsage);

    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Camera,
                     "Failed to send AudioStreamAllocate command to the camera device (NodeId: " ChipLogFormatX64
                     "). Error: %" CHIP_ERROR_FORMAT,
                     ChipLogValueX64(nodeId), error.Format());
    }

    return error;
}

CHIP_ERROR DeviceManager::DeallocateVideoStream(NodeId nodeId, uint16_t videoStreamId)
{
    ChipLogProgress(Camera, "Deallocate a video stream on the camera device.");

    CHIP_ERROR error = mAVStreamManagment.DeallocateVideoStream(nodeId, kCameraEndpointId, videoStreamId);

    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Camera,
                     "Failed to send VideoStreamDeallocate command to the camera device (NodeId: " ChipLogFormatX64
                     "). Error: %" CHIP_ERROR_FORMAT,
                     ChipLogValueX64(nodeId), error.Format());
    }
    else
    {
        // Stop the video stream process and disconnect WebRTC
        StopVideoStreamProcess(videoStreamId);
        WebRTCManager::Instance().Disconnect();

        // Remove this node's active LiveView record only when the deallocated
        // stream is the one we were tracking; this avoids dropping the record
        // when the caller explicitly deallocates a different stream id.
        auto it = mActiveLiveViewByNode.find(nodeId);
        if (it != mActiveLiveViewByNode.end() && it->second == videoStreamId)
        {
            mActiveLiveViewByNode.erase(it);
        }
    }

    return error;
}

CHIP_ERROR DeviceManager::DeallocateAudioStream(NodeId nodeId, uint16_t audioStreamId)
{
    ChipLogProgress(Camera, "Deallocate an audio stream on the camera device.");

    CHIP_ERROR error = mAVStreamManagment.DeallocateAudioStream(nodeId, kCameraEndpointId, audioStreamId);

    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Camera,
                     "Failed to send AudioStreamDeallocate command to the camera device (NodeId: " ChipLogFormatX64
                     "). Error: %" CHIP_ERROR_FORMAT,
                     ChipLogValueX64(nodeId), error.Format());
    }
    else
    {
        // Stop the audio recording pipeline. The audio process is keyed by the
        // audio stream ID, so this is the authoritative place that tears it down.
        StopAudioStreamProcess(audioStreamId);

        // Remove this node's active LiveView audio record only when the
        // deallocated stream is the one we were tracking.
        auto it = mActiveLiveViewAudioByNode.find(nodeId);
        if (it != mActiveLiveViewAudioByNode.end() && it->second == audioStreamId)
        {
            mActiveLiveViewAudioByNode.erase(it);
        }
    }

    return error;
}

void DeviceManager::HandleAttributeData(const app::ConcreteDataAttributePath & path, TLV::TLVReader & data) {}

void DeviceManager::HandleEventData(const app::EventHeader & header, TLV::TLVReader & data) {}

void DeviceManager::HandleCommandResponse(const app::ConcreteCommandPath & path, TLV::TLVReader & data)
{
    ChipLogProgress(Camera, "Command Response received.");

    if (path.mClusterId == app::Clusters::CameraAvStreamManagement::Id &&
        path.mCommandId == app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocateResponse::Id)
    {
        HandleVideoStreamAllocateResponse(data);
    }
    else if (path.mClusterId == app::Clusters::CameraAvStreamManagement::Id &&
             path.mCommandId == app::Clusters::CameraAvStreamManagement::Commands::AudioStreamAllocateResponse::Id)
    {
        HandleAudioStreamAllocateResponse(data);
    }
}

void DeviceManager::StopVideoStream(uint16_t streamId)
{
    // Disconnect WebRTC session
    WebRTCManager::Instance().Disconnect();

    // Stop the video pipeline. The audio pipeline is keyed by the audio stream ID
    // and is torn down separately via DeallocateAudioStream.
    StopVideoStreamProcess(streamId);
}

void DeviceManager::HandleVideoStreamAllocateResponse(TLV::TLVReader & data)
{
    ChipLogProgress(Camera, "Handle VideoStreamAllocateResponse command.");

    app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocateResponse::DecodableType value;
    CHIP_ERROR error = app::DataModel::Decode(data, value);

    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "Failed to decode command response value. Error: %" CHIP_ERROR_FORMAT, error.Format());
        return;
    }

    // Log all fields
    ChipLogProgress(Camera, "DecodableType fields:");
    ChipLogProgress(Camera, "  videoStreamId: %u", value.videoStreamID);

    // Store the stream ID we're setting up only for LiveView streams
    std::optional<uint16_t> audioStreamId = std::nullopt;
    if (mStreamUsage == static_cast<uint8_t>(StreamUsageEnum::kLiveView))
    {
        mPendingVideoStreamId = value.videoStreamID;

        // Only LiveView sessions allocate an audio stream; other usages (e.g. the
        // standalone WebRTC provider command) must not advertise an audio stream.
        audioStreamId = mPendingAudioStreamId;
    }

    InitiateWebRTCSession(value.videoStreamID, audioStreamId);
}

void DeviceManager::HandleAudioStreamAllocateResponse(TLV::TLVReader & data)
{
    ChipLogProgress(Camera, "Handle AudioStreamAllocateResponse command.");

    app::Clusters::CameraAvStreamManagement::Commands::AudioStreamAllocateResponse::DecodableType value;
    CHIP_ERROR error = app::DataModel::Decode(data, value);

    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "Failed to decode command response value. Error: %" CHIP_ERROR_FORMAT, error.Format());
        return;
    }

    // Log all fields
    ChipLogProgress(Camera, "DecodableType fields:");
    ChipLogProgress(Camera, "  audioStreamId: %u", value.audioStreamID);

    mPendingAudioStreamId = value.audioStreamID;

    // The audio command is still completing (OnResponse precedes OnDone, which
    // resets the shared CommandSender). Defer the VideoStreamAllocate to the
    // next event-loop iteration so the audio command can finish and release its
    // CommandSender before we reuse it; issuing it synchronously here would free
    // the in-flight sender during its own callback.
    //
    // ScheduleLambda requires a trivially-copyable closure within
    // CHIP_CONFIG_LAMBDA_EVENT_SIZE, so capture only `this` and read the
    // deferred allocation parameters from member state.
    CHIP_ERROR scheduleErr = DeviceLayer::SystemLayer().ScheduleLambda([this]() {
        CHIP_ERROR err = AllocateVideoStream(mNodeId, mStreamUsage, mOfferType, mPendingMinResWidth, mPendingMinResHeight,
                                             mPendingMinFrameRate, mPendingMinBitRate);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(Camera, "Failed to allocate video stream after audio allocation. Rolling back audio stream.");
            if (mPendingAudioStreamId.has_value())
            {
                LogErrorOnFailure(DeallocateAudioStream(mNodeId, mPendingAudioStreamId.value()));
            }
            mPendingAudioStreamId = std::nullopt;
        }
    });

    if (scheduleErr != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "Failed to schedule VideoStreamAllocate after audio allocation. Error: %" CHIP_ERROR_FORMAT,
                     scheduleErr.Format());
    }
}

void DeviceManager::InitiateWebRTCSession(uint16_t videoStreamId, std::optional<uint16_t> audioStreamId)
{
    ChipLogProgress(Camera, "DeviceManager: Initiating WebRTC session for node=0x" ChipLogFormatX64, ChipLogValueX64(mNodeId));

    // Connect to the WebRTC transport provider on the device
    CHIP_ERROR err = WebRTCManager::Instance().Connnect(*mCommissioner, mNodeId, kCameraEndpointId);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "Failed to connect WebRTC manager. Error: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    // Add a 1-second delay after successful connection to allow local SDP gets populated
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto videoStreamIdNullable = app::DataModel::MakeNullable(videoStreamId);
    auto videoStreamIdOptional = MakeOptional(videoStreamIdNullable);

    // Only advertise an audio stream when one was actually allocated for this
    // session; otherwise leave it empty so the offer is video-only.
    Optional<app::DataModel::Nullable<uint16_t>> audioStreamIdOptional = NullOptional;
    if (audioStreamId.has_value())
    {
        audioStreamIdOptional = MakeOptional(app::DataModel::MakeNullable(audioStreamId.value()));
    }

    auto streamUsage = static_cast<StreamUsageEnum>(mStreamUsage);

    // Choose between ProvideOffer and SolicitOffer based on the configured offer type
    if (mOfferType == WebRTCOfferType::kProvideOffer)
    {
        ChipLogProgress(Camera, "Using ProvideOffer for WebRTC session establishment");
        err = WebRTCManager::Instance().ProvideOffer(app::DataModel::NullNullable, // session ID (null)
                                                     streamUsage,                  // stream‑usage field
                                                     videoStreamIdOptional,        // videoStreamId you just built
                                                     audioStreamIdOptional);       // audioStreamId you just built
    }
    else // WebRTCOfferType::kSolicitOffer
    {
        ChipLogProgress(Camera, "Using SolicitOffer for WebRTC session establishment");
        err = WebRTCManager::Instance().SolicitOffer(streamUsage,            // stream‑usage field
                                                     videoStreamIdOptional,  // videoStreamId you just built
                                                     audioStreamIdOptional); // audioStreamId you just built
    }

    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "Failed to initiate WebRTC offer. Error: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

void DeviceManager::OnWebRTCSessionEstablished(uint16_t streamId)
{
    ChipLogProgress(Camera, "WebRTC session established for stream ID: %u", streamId);

    // Only start video stream process for LiveView streams
    if (mStreamUsage == static_cast<uint8_t>(StreamUsageEnum::kLiveView))
    {
        // Verify this matches our pending stream
        if (streamId == mPendingVideoStreamId)
        {
            StartVideoStreamProcess(streamId);
            mActiveLiveViewByNode[mNodeId] = streamId;

            // Start the audio pipeline (keyed by the camera-assigned audio stream
            // ID) and track that ID for this node so it can be deallocated when
            // the LiveView session is stopped.
            if (mPendingAudioStreamId.has_value())
            {
                StartAudioStreamProcess(mPendingAudioStreamId.value());
                mActiveLiveViewAudioByNode[mNodeId] = mPendingAudioStreamId.value();
            }

            mPendingVideoStreamId = 0;
            mPendingAudioStreamId = std::nullopt;
        }
    }
}

std::optional<uint16_t> DeviceManager::GetActiveLiveViewStreamId(NodeId nodeId) const
{
    auto it = mActiveLiveViewByNode.find(nodeId);
    if (it == mActiveLiveViewByNode.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<uint16_t> DeviceManager::GetActiveLiveViewAudioStreamId(NodeId nodeId) const
{
    auto it = mActiveLiveViewAudioByNode.find(nodeId);
    if (it == mActiveLiveViewAudioByNode.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void DeviceManager::StartVideoStreamProcess(uint16_t streamId)
{
    ChipLogProgress(Camera, "Starting video stream process for stream ID: %u", streamId);

    // Terminate any previous pipeline that was bound to this stream ID
    StopVideoStreamProcess(streamId);

    const uint16_t udpPort    = 5000;
    const std::string portStr = "port=" + std::to_string(udpPort);

    // Save the incoming RTP/H264 stream to a raw H.264 elementary-stream file
    // (Annex-B byte-stream) named after the stream ID. Written under /tmp to keep
    // runtime artifacts out of the working directory, consistent with the KVS,
    // interactive history, and gst log files.
    const std::string locationStr = "location=/tmp/chip_video_stream_" + std::to_string(streamId) + ".h264";

    // Per-stream log file for gst-launch stdout/stderr (built before fork so the
    // child only needs async-signal-safe calls).
    const std::string logPath = "/tmp/chip_gst_stream_" + std::to_string(streamId) + ".log";

    // Use -e so gst-launch sends an EOS on SIGINT/SIGTERM, flushing any buffered
    // data and letting filesink close the file cleanly before exiting.
    //
    // Pipeline notes:
    //   - h264parse config-interval=-1 re-inserts SPS/PPS in-band before every
    //     IDR, so the saved file is self-contained and decodable even if the
    //     recording starts mid-stream.
    //   - The explicit caps "stream-format=byte-stream,alignment=au" force an
    //     Annex-B elementary stream (00 00 00 01 start codes). Without this,
    //     rtph264depay/filesink would emit length-prefixed AVC with the SPS/PPS
    //     stranded in caps (codec_data), producing an unparseable .h264 file.
    const char * const argv[] = { "gst-launch-1.0",
                                  "-e",
                                  "udpsrc",
                                  portStr.c_str(),
                                  "!",
                                  "application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96",
                                  "!",
                                  "rtpjitterbuffer",
                                  "!",
                                  "rtph264depay",
                                  "!",
                                  "queue",
                                  "!",
                                  "h264parse",
                                  "config-interval=-1",
                                  "!",
                                  "video/x-h264,stream-format=byte-stream,alignment=au",
                                  "!",
                                  "filesink",
                                  locationStr.c_str(),
                                  nullptr };

    // Fork process to run GStreamer pipeline
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process - execute GStreamer command
        // Put the pipeline in its own process group so we can kill it safely if needed.
        setpgid(0, 0);

        // Redirect stdout and stderr to a per-stream log file so that any
        // gst-launch startup/runtime errors can be inspected after the fact.
        freopen(logPath.c_str(), "w", stdout);
        freopen(logPath.c_str(), "w", stderr);

        // Replace the child with gst‑launch‑1.0 (only returns on error)
        execvp(argv[0], const_cast<char * const *>(argv));

        // If we got here execvp failed
        ChipLogError(Camera, "execvp(gst-launch-1.0) failed: %s", strerror(errno));
        _exit(EXIT_FAILURE);
    }
    else if (pid > 0)
    {
        // Parent process - store PID for later cleanup
        mVideoStreamProcesses[streamId] = pid; // Track the real gst‑launch PID
        ChipLogProgress(Camera, "Video stream process started with PID: %d", pid);
    }
    else
    {
        // Fork failed
        ChipLogError(Camera, "Failed to fork process for video stream. Error: %s", strerror(errno));
    }
}

void DeviceManager::StopVideoStreamProcess(uint16_t streamId)
{
    auto it = mVideoStreamProcesses.find(streamId);
    if (it != mVideoStreamProcesses.end())
    {
        pid_t pid = it->second;
        ChipLogProgress(Camera, "Stopping video stream process (PID: %d) for stream ID: %u", pid, streamId);

        // Send SIGINT first so gst-launch (-e) forces an EOS and finalizes the
        // .h264 elementary-stream file (flushing h264parse/filesink) before exiting.
        if (kill(pid, SIGINT) == 0)
        {
            // Wait for graceful shutdown (with timeout)
            int status;
            int waitResult = waitpid(pid, &status, WNOHANG);

            if (waitResult == 0)
            {
                // Process still running, wait a bit then force kill
                sleep(1);
                waitResult = waitpid(pid, &status, WNOHANG);

                if (waitResult == 0)
                {
                    ChipLogProgress(Camera, "Force killing video stream process (PID: %d)", pid);
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0); // Wait for process to be reaped
                }
            }
        }
        else
        {
            ChipLogError(Camera, "Failed to send SIGINT to video process %d: %s", pid, strerror(errno));
        }

        mVideoStreamProcesses.erase(it);
    }
}

void DeviceManager::StartAudioStreamProcess(uint16_t streamId)
{
    ChipLogProgress(Camera, "Starting audio stream process for stream ID: %u", streamId);

    // Terminate any previous pipeline that was bound to this stream ID
    StopAudioStreamProcess(streamId);

    const uint16_t udpPort    = 5001; // kAudioStreamGstDestPort in WebRTCManager
    const std::string portStr = "port=" + std::to_string(udpPort);

    // Save the incoming RTP/Opus stream into an Ogg container named after the
    // stream ID. Written under /tmp to keep runtime artifacts out of the working
    // directory, consistent with the KVS, interactive history, and gst log files.
    const std::string locationStr = "location=/tmp/chip_audio_stream_" + std::to_string(streamId) + ".ogg";

    // Per-stream log file for gst-launch stdout/stderr (built before fork so the
    // child only needs async-signal-safe calls).
    const std::string logPath = "/tmp/chip_gst_audio_stream_" + std::to_string(streamId) + ".log";

    // Use -e so gst-launch sends an EOS on SIGINT/SIGTERM, flushing any buffered
    // data and letting oggmux/filesink finalize the file cleanly before exiting.
    //
    // Pipeline notes:
    //   - clock-rate=48000 is Opus' fixed internal sample rate.
    //   - rtpjitterbuffer absorbs network jitter before depayloading.
    //   - opusparse + oggmux produce a self-contained, seekable .ogg file.
    const char * const argv[] = { "gst-launch-1.0",
                                  "-e",
                                  "udpsrc",
                                  portStr.c_str(),
                                  "!",
                                  "application/x-rtp,media=audio,clock-rate=48000,encoding-name=OPUS,payload=111",
                                  "!",
                                  "rtpjitterbuffer",
                                  "!",
                                  "rtpopusdepay",
                                  "!",
                                  "opusparse",
                                  "!",
                                  "oggmux",
                                  "!",
                                  "filesink",
                                  locationStr.c_str(),
                                  nullptr };

    // Fork process to run GStreamer pipeline
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process - execute GStreamer command
        // Put the pipeline in its own process group so we can kill it safely if needed.
        setpgid(0, 0);

        // Redirect stdout and stderr to a per-stream log file so that any
        // gst-launch startup/runtime errors can be inspected after the fact.
        freopen(logPath.c_str(), "w", stdout);
        freopen(logPath.c_str(), "w", stderr);

        // Replace the child with gst‑launch‑1.0 (only returns on error)
        execvp(argv[0], const_cast<char * const *>(argv));

        // If we got here execvp failed
        ChipLogError(Camera, "execvp(gst-launch-1.0) for audio failed: %s", strerror(errno));
        _exit(EXIT_FAILURE);
    }
    else if (pid > 0)
    {
        // Parent process - store PID for later cleanup
        mAudioStreamProcesses[streamId] = pid; // Track the real gst‑launch PID
        ChipLogProgress(Camera, "Audio stream process started with PID: %d", pid);
    }
    else
    {
        // Fork failed
        ChipLogError(Camera, "Failed to fork process for audio stream. Error: %s", strerror(errno));
    }
}

void DeviceManager::StopAudioStreamProcess(uint16_t streamId)
{
    auto it = mAudioStreamProcesses.find(streamId);
    if (it != mAudioStreamProcesses.end())
    {
        pid_t pid = it->second;
        ChipLogProgress(Camera, "Stopping audio stream process (PID: %d) for stream ID: %u", pid, streamId);

        // Send SIGINT first so gst-launch (-e) forces an EOS and finalizes the
        // Ogg file before exiting.
        if (kill(pid, SIGINT) == 0)
        {
            // Wait for graceful shutdown (with timeout)
            int status;
            int waitResult = waitpid(pid, &status, WNOHANG);

            if (waitResult == 0)
            {
                // Process still running, wait a bit then force kill
                sleep(1);
                waitResult = waitpid(pid, &status, WNOHANG);

                if (waitResult == 0)
                {
                    ChipLogProgress(Camera, "Force killing audio stream process (PID: %d)", pid);
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0); // Wait for process to be reaped
                }
            }
        }
        else
        {
            ChipLogError(Camera, "Failed to send SIGINT to audio process %d: %s", pid, strerror(errno));
        }

        mAudioStreamProcesses.erase(it);
    }
}

} // namespace camera
