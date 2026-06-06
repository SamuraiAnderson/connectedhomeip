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

#pragma once

#include <app-common/zap-generated/cluster-objects.h>
#include <device-manager/AVStreamManagement.h>
#include <platform/CHIPDeviceLayer.h>

#include <map>
#include <optional>

namespace camera {

enum class WebRTCOfferType : uint8_t
{
    kProvideOffer = 0,
    kSolicitOffer = 1
};

class DeviceManager
{
public:
    DeviceManager() = default;

    static DeviceManager & Instance()
    {
        static DeviceManager instance;
        return instance;
    }

    static void VideoStreamSignalHandler(int sig);

    CHIP_ERROR Init(chip::Controller::DeviceCommissioner * commissioner);

    void Shutdown();

    /**
     * @brief Sends a VideoStreamAllocate command to the device.
     *
     * @param nodeId       The node ID of the remote camera device.
     * @param streamUsage  The usage of the stream(Recording, LiveView, etc) that this allocation is for.
     * @param offerType    The type of WebRTC offer to use (ProvideOffer or SolicitOffer).
     * @param minResWidth  Optional minimum width for the video stream. If not specified, uses default value.
     * @param minResHeight Optional minimum height for the video stream. If not specified, uses default value.
     * @param minFrameRate Optional minimum frame rate for the video stream. If not specified, uses default value.
     * @param minBitRate   Optional minimum bit rate for the video stream. If not specified, uses default value.
     * @return CHIP_ERROR  CHIP_NO_ERROR on success, or an appropriate error code on failure.
     */
    CHIP_ERROR AllocateVideoStream(chip::NodeId nodeId, uint8_t streamUsage,
                                   WebRTCOfferType offerType             = WebRTCOfferType::kProvideOffer,
                                   chip::Optional<uint16_t> minResWidth  = chip::NullOptional,
                                   chip::Optional<uint16_t> minResHeight = chip::NullOptional,
                                   chip::Optional<uint16_t> minFrameRate = chip::NullOptional,
                                   chip::Optional<uint32_t> minBitRate   = chip::NullOptional);

    /**
     * @brief Starts a LiveView session that streams both audio and video.
     *
     * This first sends an AudioStreamAllocate command; once the camera responds
     * with an audio stream ID, a VideoStreamAllocate command is sent, and a
     * WebRTC session carrying both streams is established. The video allocation
     * parameters are deferred and applied to the subsequent VideoStreamAllocate.
     *
     * @param nodeId       The node ID of the remote camera device.
     * @param streamUsage  The usage of the stream(Recording, LiveView, etc) that this allocation is for.
     * @param offerType    The type of WebRTC offer to use (ProvideOffer or SolicitOffer).
     * @param minResWidth  Optional minimum width for the video stream. If not specified, uses default value.
     * @param minResHeight Optional minimum height for the video stream. If not specified, uses default value.
     * @param minFrameRate Optional minimum frame rate for the video stream. If not specified, uses default value.
     * @param minBitRate   Optional minimum bit rate for the video stream. If not specified, uses default value.
     * @return CHIP_ERROR  CHIP_NO_ERROR on success, or an appropriate error code on failure.
     */
    CHIP_ERROR AllocateLiveViewStream(chip::NodeId nodeId, uint8_t streamUsage,
                                      WebRTCOfferType offerType             = WebRTCOfferType::kProvideOffer,
                                      chip::Optional<uint16_t> minResWidth  = chip::NullOptional,
                                      chip::Optional<uint16_t> minResHeight = chip::NullOptional,
                                      chip::Optional<uint16_t> minFrameRate = chip::NullOptional,
                                      chip::Optional<uint32_t> minBitRate   = chip::NullOptional);

    /**
     * @brief Sends a VideoStreamDeallocate command to the device.
     *
     * @param nodeId        The node ID of the remote camera device.
     * @param videoStreamId The VideoStreamID for the stream to be deallocated.
     * @return CHIP_ERROR   CHIP_NO_ERROR on success, or an appropriate error code on failure.
     */
    CHIP_ERROR DeallocateVideoStream(chip::NodeId nodeId, uint16_t videoStreamId);

    /**
     * @brief Sends an AudioStreamDeallocate command to the device.
     *
     * @param nodeId        The node ID of the remote camera device.
     * @param audioStreamId The AudioStreamID for the stream to be deallocated.
     * @return CHIP_ERROR   CHIP_NO_ERROR on success, or an appropriate error code on failure.
     */
    CHIP_ERROR DeallocateAudioStream(chip::NodeId nodeId, uint16_t audioStreamId);

    void HandleAttributeData(const chip::app::ConcreteDataAttributePath & path, chip::TLV::TLVReader & data);

    void HandleEventData(const chip::app::EventHeader & header, chip::TLV::TLVReader & data);

    void HandleCommandResponse(const chip::app::ConcreteCommandPath & path, chip::TLV::TLVReader & data);

    void StopVideoStream(uint16_t streamId);

    /**
     * @brief Callback invoked when WebRTC session is established
     *
     * @param streamId The video stream ID for which the session was established
     */
    void OnWebRTCSessionEstablished(uint16_t streamId);

    /**
     * @brief Look up the currently active LiveView video stream ID for a given node.
     *
     * Returns the stream ID recorded when a LiveView WebRTC session was last
     * established for that node, or std::nullopt if no LiveView stream is
     * currently tracked for it.
     *
     * @param nodeId The node ID of the remote camera device.
     */
    std::optional<uint16_t> GetActiveLiveViewStreamId(chip::NodeId nodeId) const;

    /**
     * @brief Look up the currently active LiveView audio stream ID for a given node.
     *
     * Returns the audio stream ID recorded when a LiveView WebRTC session was
     * last established for that node, or std::nullopt if no LiveView audio
     * stream is currently tracked for it.
     *
     * @param nodeId The node ID of the remote camera device.
     */
    std::optional<uint16_t> GetActiveLiveViewAudioStreamId(chip::NodeId nodeId) const;

private:
    chip::Controller::DeviceCommissioner * mCommissioner = nullptr;
    chip::NodeId mNodeId                                 = chip::kUndefinedNodeId;
    uint8_t mStreamUsage                                 = 0;
    WebRTCOfferType mOfferType                           = WebRTCOfferType::kProvideOffer;
    std::map<uint16_t, pid_t> mVideoStreamProcesses;             // Video stream ID -> Video Process ID mapping
    std::map<uint16_t, pid_t> mAudioStreamProcesses;             // Audio stream ID -> Audio Process ID mapping
    std::map<chip::NodeId, uint16_t> mActiveLiveViewByNode;      // Node ID -> active LiveView video stream ID
    std::map<chip::NodeId, uint16_t> mActiveLiveViewAudioByNode; // Node ID -> active LiveView audio stream ID
    uint16_t mPendingVideoStreamId = 0;                          // Track the video stream ID we're setting up

    // Audio stream ID allocated for the in-flight LiveView session, or
    // std::nullopt when the current allocation carries no audio (e.g. the
    // standalone WebRTC provider command).
    std::optional<uint16_t> mPendingAudioStreamId;

    // Deferred VideoStreamAllocate parameters captured by AllocateLiveViewStream
    // and applied once the AudioStreamAllocate response arrives.
    chip::Optional<uint16_t> mPendingMinResWidth;
    chip::Optional<uint16_t> mPendingMinResHeight;
    chip::Optional<uint16_t> mPendingMinFrameRate;
    chip::Optional<uint32_t> mPendingMinBitRate;

    AVStreamManagement mAVStreamManagment;

    void HandleVideoStreamAllocateResponse(chip::TLV::TLVReader & data);
    void HandleAudioStreamAllocateResponse(chip::TLV::TLVReader & data);
    void InitiateWebRTCSession(uint16_t videoStreamId, std::optional<uint16_t> audioStreamId);
    void StartVideoStreamProcess(uint16_t streamId);
    void StopVideoStreamProcess(uint16_t streamId);
    void StartAudioStreamProcess(uint16_t streamId);
    void StopAudioStreamProcess(uint16_t streamId);
};

} // namespace camera
