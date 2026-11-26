#include "sandbox_stream_codec.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace sandbox_stream {

namespace {

uint64_t fnv1a_block(const uint8_t *ptr, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= ptr[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void write_u16(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

bool read_u16(const uint8_t *p, size_t &off, size_t len, uint16_t &out) {
    if (off + 2 > len) return false;
    out = static_cast<uint16_t>((static_cast<uint16_t>(p[off]) << 8) | p[off + 1]);
    off += 2;
    return true;
}

}  // namespace

bool encode_tile_frame(TileEncoderState &st,
                       const uint8_t *src_bgra,
                       int src_w,
                       int src_h,
                       int src_stride,
                       bool force_keyframe,
                       int target_max_w,
                       std::vector<uint8_t> &out_packet) {
    if (!src_bgra || src_w <= 0 || src_h <= 0 || src_stride < src_w * 4) return false;

    int dst_w = src_w;
    int dst_h = src_h;
    if (target_max_w > 0 && src_w > target_max_w) {
        dst_w = target_max_w;
        dst_h = std::max(1, (src_h * dst_w) / src_w);
    }

    bool need_key = force_keyframe || (st.width != dst_w || st.height != dst_h);
    if (need_key) {
        st.width = dst_w;
        st.height = dst_h;
        st.prev_hashes.clear();
    }

    // Prepare scaled BGRA buffer (direct view if 1:1)
    std::vector<uint8_t> scaled;
    const uint8_t *frame_ptr = src_bgra;
    int frame_stride = src_stride;
    if (dst_w != src_w || dst_h != src_h) {
        scaled.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 4);
        for (int y = 0; y < dst_h; ++y) {
            int sy = (y * src_h) / dst_h;
            const uint32_t *srow = reinterpret_cast<const uint32_t *>(src_bgra + static_cast<size_t>(sy) * src_stride);
            uint32_t *drow = reinterpret_cast<uint32_t *>(scaled.data() + static_cast<size_t>(y) * dst_w * 4);
            for (int x = 0; x < dst_w; ++x) {
                int sx = (x * src_w) / dst_w;
                drow[x] = srow[sx];
            }
        }
        frame_ptr = scaled.data();
        frame_stride = dst_w * 4;
    }

    const int ts = st.tile_size;
    const int cols = (dst_w + ts - 1) / ts;
    const int rows = (dst_h + ts - 1) / ts;
    if (static_cast<int>(st.prev_hashes.size()) != cols * rows) {
        st.prev_hashes.assign(static_cast<size_t>(cols * rows), 0ULL);
        need_key = true;
    }

    std::vector<uint8_t> raw_tiles;
    uint16_t changed_count = 0;

    for (int ty = 0; ty < rows; ++ty) {
        int y0 = ty * ts;
        int th = std::min(ts, dst_h - y0);
        for (int tx = 0; tx < cols; ++tx) {
            int x0 = tx * ts;
            int tw = std::min(ts, dst_w - x0);
            uint64_t h = 14695981039346656037ULL;
            for (int r = 0; r < th; ++r) {
                const uint8_t *rowp = frame_ptr + static_cast<size_t>(y0 + r) * frame_stride + static_cast<size_t>(x0) * 4;
                h ^= fnv1a_block(rowp, static_cast<size_t>(tw) * 4);
                h *= 1099511628211ULL;
            }
            size_t idx = static_cast<size_t>(ty * cols + tx);
            if (!need_key && st.prev_hashes[idx] == h) continue;
            st.prev_hashes[idx] = h;
            ++changed_count;

            write_u16(raw_tiles, static_cast<uint16_t>(tx));
            write_u16(raw_tiles, static_cast<uint16_t>(ty));
            size_t old_sz = raw_tiles.size();
            raw_tiles.resize(old_sz + static_cast<size_t>(tw * th * 4));
            uint8_t *dst_p = raw_tiles.data() + old_sz;
            for (int r = 0; r < th; ++r) {
                const uint8_t *rowp = frame_ptr + static_cast<size_t>(y0 + r) * frame_stride + static_cast<size_t>(x0) * 4;
                std::memcpy(dst_p + static_cast<size_t>(r) * tw * 4, rowp, static_cast<size_t>(tw) * 4);
            }
        }
    }

    if (changed_count == 0) return false;

    uLongf comp_bound = compressBound(static_cast<uLong>(raw_tiles.size()));
    out_packet.clear();
    out_packet.reserve(11 + comp_bound);
    write_u16(out_packet, static_cast<uint16_t>(dst_w));
    write_u16(out_packet, static_cast<uint16_t>(dst_h));
    out_packet.push_back(need_key ? 1 : 0);
    write_u16(out_packet, static_cast<uint16_t>(ts));
    write_u16(out_packet, changed_count);
    // Uncompressed length (u32 BE)
    uint32_t raw_len = static_cast<uint32_t>(raw_tiles.size());
    out_packet.push_back(static_cast<uint8_t>((raw_len >> 24) & 0xFF));
    out_packet.push_back(static_cast<uint8_t>((raw_len >> 16) & 0xFF));
    out_packet.push_back(static_cast<uint8_t>((raw_len >> 8) & 0xFF));
    out_packet.push_back(static_cast<uint8_t>(raw_len & 0xFF));

    size_t hdr_sz = out_packet.size();
    out_packet.resize(hdr_sz + comp_bound);
    uLongf actual_comp = comp_bound;
    int zrc = compress2(out_packet.data() + hdr_sz, &actual_comp,
                        raw_tiles.data(), static_cast<uLong>(raw_tiles.size()), 1);
    if (zrc != Z_OK) return false;
    out_packet.resize(hdr_sz + actual_comp);
    ++st.frame_seq;
    return true;
}

bool decode_tile_frame(TileDecoderState &st, const uint8_t *packet, size_t len) {
    if (!packet || len < 13) return false;
    size_t off = 0;
    uint16_t w = 0, h = 0, ts = 0, count = 0;
    if (!read_u16(packet, off, len, w) || !read_u16(packet, off, len, h)) return false;
    uint8_t flags = packet[off++];
    if (!read_u16(packet, off, len, ts) || !read_u16(packet, off, len, count)) return false;
    if (off + 4 > len || w == 0 || h == 0 || w > 4096 || h > 4096 || ts == 0) return false;
    uint32_t raw_len = (static_cast<uint32_t>(packet[off]) << 24) |
                       (static_cast<uint32_t>(packet[off + 1]) << 16) |
                       (static_cast<uint32_t>(packet[off + 2]) << 8) |
                       static_cast<uint32_t>(packet[off + 3]);
    off += 4;
    if (raw_len == 0 || raw_len > 64u * 1024u * 1024u) return false;

    std::vector<uint8_t> raw(raw_len);
    uLongf dst_len = raw_len;
    if (uncompress(raw.data(), &dst_len, packet + off, static_cast<uLong>(len - off)) != Z_OK ||
        dst_len != raw_len) {
        return false;
    }

    if ((flags & 1) || st.width != w || st.height != h ||
        st.bgra.size() != static_cast<size_t>(w) * h * 4) {
        st.width = w;
        st.height = h;
        st.bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    }

    const int cols = (w + ts - 1) / ts;
    const int rows = (h + ts - 1) / ts;
    size_t roff = 0;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t tx = 0, ty = 0;
        if (!read_u16(raw.data(), roff, raw.size(), tx) ||
            !read_u16(raw.data(), roff, raw.size(), ty)) {
            return false;
        }
        if (tx >= cols || ty >= rows) return false;
        int x0 = tx * ts;
        int y0 = ty * ts;
        int tw = std::min(static_cast<int>(ts), w - x0);
        int th = std::min(static_cast<int>(ts), h - y0);
        size_t bytes = static_cast<size_t>(tw) * th * 4;
        if (roff + bytes > raw.size()) return false;
        for (int r = 0; r < th; ++r) {
            uint8_t *dst_row = st.bgra.data() + (static_cast<size_t>(y0 + r) * w + x0) * 4;
            std::memcpy(dst_row, raw.data() + roff + static_cast<size_t>(r) * tw * 4, static_cast<size_t>(tw) * 4);
        }
        roff += bytes;
    }
    return true;
}

}  // namespace sandbox_stream
