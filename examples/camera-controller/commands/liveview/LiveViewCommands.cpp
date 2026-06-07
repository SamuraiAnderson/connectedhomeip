/*
 *   Copyright (c) 2025 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include "LiveViewCommands.h"
#include <commands/interactive/InteractiveCommands.h>
#include <device-manager/DeviceManager.h>

using namespace chip;

namespace {} // namespace

CHIP_ERROR LiveViewStartCommand::RunCommand()
{
    ChipLogProgress(Camera, "Run LiveViewStartCommand");

    if (mPeerNodeId == chip::kUndefinedNodeId)
    {
        ChipLogError(Camera, "Missing --node-id");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    // Use provided stream usage or default to 3 (LiveView)
    uint8_t streamUsage = mStreamUsage.HasValue() ? mStreamUsage.Value() : 3;

    return camera::DeviceManager::Instance().AllocateLiveViewStream(mPeerNodeId, streamUsage,
                                                                    camera::WebRTCOfferType::kProvideOffer, mMinResWidth,
                                                                    mMinResHeight, mMinFrameRate, mMinBitRate, mAudioSampleRate);
}

CHIP_ERROR LiveViewStopCommand::RunCommand()
{
    ChipLogProgress(Camera, "Run LiveViewStopCommand");

    uint16_t effectiveStreamId = 0;
    if (mVideoStreamID.HasValue())
    {
        effectiveStreamId = mVideoStreamID.Value();
    }
    else
    {
        auto activeStreamId = camera::DeviceManager::Instance().GetActiveLiveViewStreamId(mPeerNodeId);
        if (!activeStreamId.has_value())
        {
            ChipLogError(Camera,
                         "No active LiveView stream tracked for node 0x" ChipLogFormatX64
                         "; please pass video-stream-id explicitly",
                         ChipLogValueX64(mPeerNodeId));
            return CHIP_ERROR_NOT_FOUND;
        }
        effectiveStreamId = activeStreamId.value();
        ChipLogProgress(Camera, "Using active LiveView stream id %u for node 0x" ChipLogFormatX64, effectiveStreamId,
                        ChipLogValueX64(mPeerNodeId));
    }

    // Capture the active audio stream id (if any) before deallocating the video
    // stream, since DeallocateVideoStream may clear this node's tracking state.
    auto activeAudioStreamId = camera::DeviceManager::Instance().GetActiveLiveViewAudioStreamId(mPeerNodeId);

    camera::DeviceManager::Instance().StopVideoStream(effectiveStreamId);

    CHIP_ERROR error = camera::DeviceManager::Instance().DeallocateVideoStream(mPeerNodeId, effectiveStreamId);

    // Tear down the associated audio stream, if one was allocated for this node.
    if (activeAudioStreamId.has_value())
    {
        CHIP_ERROR audioError = camera::DeviceManager::Instance().DeallocateAudioStream(mPeerNodeId, activeAudioStreamId.value());
        if (audioError != CHIP_NO_ERROR && error == CHIP_NO_ERROR)
        {
            error = audioError;
        }
    }

    return error;
}
