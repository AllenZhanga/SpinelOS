#pragma once

#include <cstdint>
#include <vector>

namespace sandbox_stream {

enum MsgType : uint8_t {
    MSG_FRAME_TILE = 1,
    MSG_MOUSE_MOVE = 2,
    MSG_MOUSE_BTN  = 3,
    MSG_MOUSE_AXIS = 4,
    MSG_KEY_EVENT  = 5,
    MSG_SET_MODE   = 6,   // 0=preview (scaled), 1=takeover (full)
    MSG_CLIP_TEXT  = 7,
    MSG_LAUNCH_APP = 8,
    MSG_KEYFRAME_REQ = 9,
    MSG_BYE        = 10,
    MSG_AI_STATE   = 11,
    MSG_AI_ACTION  = 12,
};

struct TileEncoderState {
    int width = 0;
    int height = 0;
    int tile_size = 64;
    std::vector<uint64_t> prev_hashes;
    uint32_t frame_seq = 0;

    void reset() {
        width = 0;
        height = 0;
        prev_hashes.clear();
    }
};

struct TileDecoderState {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4

    void reset() {
        width = 0;
        height = 0;
        bgra.clear();
    }
};

// Encodes BGRA32 input frame (optionally downscaled to preview_max_w) into a zlib-compressed dirty-tile packet.
// Returns false if 0 tiles changed (idle screen -> send 0 bytes!).
bool encode_tile_frame(TileEncoderState &st,
                       const uint8_t *src_bgra,
                       int src_w,
                       int src_h,
                       int src_stride,
                       bool force_keyframe,
                       int target_max_w,
                       std::vector<uint8_t> &out_packet);

// Decodes MSG_FRAME_TILE packet into TileDecoderState::bgra.
bool decode_tile_frame(TileDecoderState &st, const uint8_t *packet, size_t len);

}  // namespace sandbox_stream
