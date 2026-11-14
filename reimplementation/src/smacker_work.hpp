#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace swiv3d::detail {
namespace smacker_work {

using CacheSlack = std::array<std::array<std::uint32_t,3>,4>;

// Normal-core instruction work for the original 0x6b500..0x6bd3f builders.
// Destination slack starts clear. Each source bit and lookup branch contributes
// its translated instruction path, independently of host decoder performance.
class Tables {
public:
    explicit Tables(const std::vector<std::uint8_t>& bytes, CacheSlack slack = {})
        : bytes_(bytes), slack_(slack) {}

    std::uint64_t work() {
        constexpr unsigned header_bytes = 104;
        const auto frames = word(12) + (word(20) & 1);
        position_ = std::uint64_t(header_bytes + std::uint64_t(frames) * 5) * 8;
        end_ = position_ + std::uint64_t(word(52)) * 8;
        if (end_ > std::uint64_t(bytes_.size()) * 8) { throw std::runtime_error("Truncated movie tables"); }
        origin_ = position_;
        std::uint64_t nodes = 0;
        constexpr std::array<unsigned,4> lookup_bits{11,10,12,8};
        for (unsigned tree = 0; tree < lookup_bits.size(); ++tree) {
            const auto size = word(56 + tree * 4);
            if (size < 12 || size % 4 != 0) { throw std::runtime_error("Unsupported movie tree capacity"); }
            nodes += size / 4;
            trees_[tree] = build(lookup_bits[tree],(size - 12) / 4);
            for (auto value : slack_[tree]) {
                if ((value & 1) != 0) { instructions_ += 8; values_[value >> 16] = true; }
            }
            if ((slack_[tree].back() & 1) != 0) { instructions_ -= 2; }
        }
        unsigned missing = 0;
        while (missing < values_.size() && values_[missing]) { ++missing; }
        if (missing == values_.size()) { throw std::runtime_error("Movie tables exhaust fallback values"); }
        const auto scans = missing / 32 + 1;
        // Main routine, value-marking and pointer-fixup sweeps. Four trees each
        // reserve three clear cache slots beyond their serialized nodes.
        constexpr unsigned main_instructions = 144, sweep_setup = 36;
        constexpr unsigned copy_dwords = 16, clear_dwords = 2048;
        return instructions_ + main_instructions + sweep_setup + nodes * 11 + leaves_ * 8 + 8 +
            repeated(copy_dwords) + repeated(clear_dwords) + repeated(scans);
    }

    std::uint64_t first_video_work() {
        // Empty synthetic tables have no qualified original video-work path.
        if (word(56) == 0 || word(60) == 0 || word(64) == 0 || word(68) == 0) { return 0; }
        work();
        const auto frames = std::uint64_t(word(12)) + (word(20) & 1);
        auto offset = 104 + frames * 5 + word(52);
        const auto frame_bytes = word(104) & ~3u;
        if (offset > bytes_.size() || frame_bytes > bytes_.size() - offset) {
            throw std::runtime_error("Truncated first video frame");
        }
        const auto end = offset + frame_bytes;
        const auto type = bytes_.at(static_cast<std::size_t>(104 + frames * 4));
        if ((type & 1) != 0) { offset += std::uint64_t(bytes_.at(static_cast<std::size_t>(offset))) * 4; }
        for (unsigned track = 0; track < 7; ++track) {
            if ((type & (2u << track)) == 0) { continue; }
            if (offset > end || end - offset < 4) { throw std::runtime_error("Truncated video audio packet"); }
            const auto length = word(static_cast<std::size_t>(offset));
            if (length < 4 || length > end - offset) { throw std::runtime_error("Invalid video audio packet"); }
            offset += length;
        }
        if (offset > end) { throw std::runtime_error("Truncated video palette"); }
        if (offset == end) { return 0; }
        position_ = offset * 8;
        end_ = end * 8;
        instructions_ = 175; // Decoder setup, cache reset and common return.
        available_ = 32;
        const auto width = word(4) / 4;
        const auto rows = word(8) / 4;
        if (width == 0 || rows == 0 || word(4) % 4 != 0 || word(8) % 4 != 0) {
            throw std::runtime_error("Unsupported video block geometry");
        }
        std::uint64_t block = 0, dirty_end = 0, marked = 0;
        unsigned row_left = width;
        const auto total = std::uint64_t(width) * rows;
        while (block < total) {
            const auto value = video_symbol(3,0);
            instructions_ += 7;
            const auto kind = value & 3;
            const auto code = (value >> 2) & 63;
            auto length = code < 59 ? code + 1 : 128u << (code - 59);
            if (length > total - block) { throw std::runtime_error("Video run exceeds frame"); }
            mark_video(length,kind,dirty_end,marked);
            if (kind == 2 || kind == 3) {
                if (kind == 3) { instructions_ += 4; }
                while (length != 0) {
                    const auto amount = std::min(length,row_left);
                    instructions_ += (kind == 3 ? 7 : 5) + (length < row_left);
                    if (kind == 3) { instructions_ += repeated(amount) * 4 + 9; }
                    block += amount;
                    row_left -= amount;
                    length -= amount;
                    if (row_left == 0) {
                        instructions_ += 2;
                        if (block == total) { break; }
                        instructions_ += 5;
                        row_left = width;
                    } else { instructions_ += 2; }
                    if (length == 0) { ++instructions_; }
                }
                continue;
            }
            for (unsigned index = 0; index < length; ++index) {
                if (kind == 0) {
                    video_symbol(1,0);
                    const auto mask = video_symbol(0,0);
                    instructions_ += 20 + mono_work_[mask & 255] + mono_work_[mask >> 8];
                } else {
                    for (unsigned word_index = 0; word_index < 8; ++word_index) { video_symbol(2,word_index); }
                    instructions_ += 10;
                }
                ++block;
                if (--row_left == 0) {
                    instructions_ += 2;
                    if (block == total) { break; }
                    instructions_ += 5;
                    row_left = width;
                } else { instructions_ += 2; }
            }
            if (block < total) { ++instructions_; }
        }
        return instructions_;
    }

private:
    struct Node { int left = -1, right = -1; unsigned value = 0; int cache_slot = -1; };
    using Tree = std::vector<Node>;

    static std::uint64_t repeated(std::uint64_t count) { return count + (count + 3) / 4; }

    std::uint32_t word(std::size_t offset) const {
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) { value |= std::uint32_t(bytes_.at(offset + byte)) << (byte * 8); }
        return value;
    }

    unsigned read(unsigned count) {
        if (count > end_ - position_) { throw std::runtime_error("Movie tree bitstream exhausted"); }
        unsigned value = 0;
        for (unsigned bit = 0; bit < count; ++bit, ++position_) {
            value |= ((bytes_.at(position_ / 8) >> (position_ % 8)) & 1u) << bit;
        }
        return value;
    }

    unsigned bit() {
        instructions_ += 4 + ((position_ - origin_) % 32 == 0 ? 3 : 0);
        return read(1);
    }

    unsigned byte() {
        const auto phase = (position_ - origin_) % 32;
        instructions_ += phase != 0 && phase <= 24 ? 7 : 15;
        return read(8);
    }

    void end_tree() {
        if (bit() != 0) { throw std::runtime_error("Invalid movie tree terminator"); }
    }

    int small_node(Tree& tree, unsigned depth) {
        if (tree.size() >= 511 || depth > 256) { throw std::runtime_error("Movie byte tree exceeds capacity"); }
        const auto index = static_cast<int>(tree.size());
        tree.push_back({});
        if (bit() != 0) {
            instructions_ += 11;
            tree[index].left = small_node(tree,depth + 1);
            tree[index].right = small_node(tree,depth + 1);
        } else {
            instructions_ += 9;
            tree[index].value = byte();
        }
        return index;
    }

    Tree small_tree() {
        Tree tree;
        instructions_ += 3;
        small_node(tree,0);
        end_tree();
        instructions_ += 4;
        return tree;
    }

    unsigned symbol(const Tree& tree) {
        instructions_ += 4;
        int index = 0;
        while (tree.at(index).left >= 0) {
            const auto direction = bit();
            instructions_ += 4 + (direction == 0);
            index = direction == 0 ? tree[index].left : tree[index].right;
        }
        return tree.at(index).value;
    }

    int large_node(Tree& tree, const Tree& low, const Tree& high,
                   const std::array<unsigned,3>& cache, std::array<bool,3>& found,
                   unsigned capacity, unsigned depth) {
        if (tree.size() >= capacity || depth > 256) { throw std::runtime_error("Movie word tree exceeds capacity"); }
        const auto index = static_cast<int>(tree.size());
        tree.push_back({});
        if (bit() != 0) {
            instructions_ += 12;
            tree[index].left = large_node(tree,low,high,cache,found,capacity,depth + 1);
            tree[index].right = large_node(tree,low,high,cache,found,capacity,depth + 1);
            return index;
        }
        instructions_ += 17;
        auto value = symbol(low);
        value |= symbol(high) << 8;
        ++leaves_;
        unsigned slot = 0;
        while (slot < cache.size() && cache[slot] != value) { ++slot; }
        constexpr std::array<unsigned,4> cache_instructions{5,7,8,6};
        instructions_ += cache_instructions[slot];
        if (slot < cache.size()) { found[slot] = true; tree[index].cache_slot = static_cast<int>(slot); value = 0; }
        values_[value] = true;
        tree[index].value = value;
        return index;
    }

    void lookup(const Tree& tree, unsigned bits) {
        const unsigned count = 1u << bits;
        instructions_ += 5 + count * 17;
        for (unsigned prefix = 0; prefix < count; ++prefix) {
            unsigned depth = 0;
            int index = 0;
            while (tree.at(index).left >= 0 && depth < bits) {
                const auto direction = (prefix >> depth++) & 1;
                instructions_ += 11 + (direction == 0);
                index = direction == 0 ? tree[index].left : tree[index].right;
            }
            if (tree.at(index).left >= 0) { instructions_ += 4; }
        }
    }

    Tree build(unsigned bits, unsigned capacity) {
        if (bit() == 0) { instructions_ += 6; return Tree(1); }
        instructions_ += 11;
        Tree low(1), high(1);
        if (bit() != 0) { instructions_ += 2; low = small_tree(); }
        if (bit() != 0) { instructions_ += 2; high = small_tree(); }
        std::array<unsigned,3> cache{};
        for (auto& value : cache) { value = byte(); value |= byte() << 8; }
        instructions_ += 15;
        Tree tree;
        std::array<bool,3> found{};
        large_node(tree,low,high,cache,found,capacity,0);
        if (tree.size() != capacity) { throw std::runtime_error("Incomplete movie word tree"); }
        end_tree();
        instructions_ += 10 + !found[0] + (!found[1] + !found[2]) * 2;
        lookup(tree,bits);
        return tree;
    }

    unsigned video_symbol(unsigned tree_index, unsigned word_index) {
        constexpr std::array<unsigned,4> prefix_bits{11,10,12,8};
        const auto bits = prefix_bits[tree_index];
        const bool refill = available_ < bits;
        instructions_ += 4 + (refill ? 18 : 11);
        if (refill) { available_ += 16; }
        const auto& tree = trees_[tree_index];
        int node = 0;
        unsigned depth = 0;
        while (tree.at(node).left >= 0 && depth < bits) {
            node = read(1) == 0 ? tree[node].left : tree[node].right;
            ++depth;
            --available_;
        }
        if (tree.at(node).left >= 0 && refill) { ++instructions_; }
        while (tree.at(node).left >= 0) {
            instructions_ += 10;
            if (available_ == 0) { available_ = 32; instructions_ += 3; }
            const auto direction = read(1);
            --available_;
            instructions_ += direction == 0;
            node = direction == 0 ? tree[node].left : tree[node].right;
        }
        const auto& leaf = tree.at(node);
        const auto value = leaf.cache_slot < 0 ? leaf.value : video_cache_[tree_index][leaf.cache_slot];
        instructions_ += tree_index == 0 || tree_index == 3 || word_index % 2 != 0 ? 7 : 5;
        auto& cache = video_cache_[tree_index];
        if (value != cache[0]) {
            instructions_ += 9;
            cache = {value,cache[0],cache[1]};
        }
        return value;
    }

    void mark_video(unsigned length, unsigned kind, std::uint64_t& end, std::uint64_t& marked) {
        end += length;
        instructions_ += kind == 2 ? 6 : 4;
        if (marked > end) { return; }
        if (kind == 2) {
            instructions_ += 4;
            const auto next = end / 16;
            if (marked / 16 < next) { instructions_ += 3 + (next - marked / 16) * 4; }
            instructions_ += 2;
            marked = next * 16;
            return;
        }
        instructions_ += 6;
        const auto next = (end + 15) / 16;
        if (marked / 16 != next) { instructions_ += 3 + (next - marked / 16) * 4; }
        instructions_ += 5;
        marked = next * 16;
    }

    // Scalar instruction charges of the original two-row monochrome selectors.
    static constexpr std::array<unsigned char,256> mono_work_{
        6,6,6,6,7,7,7,7,6,6,6,6,7,7,7,7,
        6,5,6,6,6,6,7,6,5,6,5,5,7,6,6,7,
        6,6,5,6,6,6,7,6,5,6,5,5,7,6,6,7,
        6,6,6,6,7,6,6,7,6,6,6,6,7,7,7,7,
        7,6,6,7,5,5,6,5,5,6,5,5,7,6,6,7,
        7,6,6,6,5,4,5,5,5,6,4,5,6,6,6,7,
        7,7,7,6,6,5,5,6,6,5,5,6,6,7,7,7,
        7,6,6,7,5,5,6,5,5,6,5,5,7,6,6,7,
        6,5,5,6,5,5,6,5,4,5,4,4,6,5,5,6,
        6,6,6,6,6,5,5,6,5,5,5,5,6,6,6,6,
        6,5,5,6,5,4,5,5,4,5,3,4,6,5,5,6,
        6,5,5,6,5,5,6,5,4,5,4,4,6,5,5,6,
        7,7,7,7,7,6,6,7,6,6,6,6,6,6,6,6,
        7,6,6,7,6,6,7,6,5,6,5,5,6,5,6,6,
        7,6,6,7,6,6,7,6,5,6,5,5,6,6,5,6,
        7,7,7,7,7,7,7,7,6,6,6,6,6,6,6,6,
    };
    std::array<Tree,4> trees_;
    std::array<std::array<unsigned,3>,4> video_cache_{};
    unsigned available_ = 32;

    const std::vector<std::uint8_t>& bytes_;
    CacheSlack slack_;
    std::uint64_t position_ = 0, origin_ = 0, end_ = 0, instructions_ = 0, leaves_ = 0;
    std::array<bool,65536> values_{};
};

// Original 0x6e9f0 stereo, 8-bit DPCM path. Trees and sample bits determine
// normal-core work. Ring wrapping adds separate caller-state costs.
class StereoPcm {
public:
    StereoPcm(const std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::size_t size, std::uint32_t decoded_bytes)
        : bytes_(bytes), offset_(offset), size_(size), decoded_bytes_(decoded_bytes) {}

    std::uint64_t work() {
        if (offset_ > bytes_.size() || size_ > bytes_.size() - offset_ ||
            decoded_bytes_ == 0 || decoded_bytes_ % 2 != 0) {
            throw std::runtime_error("Invalid stereo movie PCM extent");
        }
        if (bit() != 1 || bit() != 1 || bit() != 0) {
            throw std::runtime_error("Unsupported movie PCM work format");
        }
        const auto left = tree();
        const auto right = tree();
        byte();
        byte();
        for (std::uint32_t sample = 1; sample < decoded_bytes_ / 2; ++sample) {
            symbol(left);
            symbol(right);
            // Two channel deltas, destination advance and ring-end test.
            instructions_ += 10;
        }
        // Entry, tree dispatch, initial destination and final register restore.
        constexpr unsigned setup_and_return = 44, final_sample = 4;
        return instructions_ + setup_and_return + final_sample;
    }

private:
    struct Node { int left = -1, right = -1; };
    using Tree = std::vector<Node>;

    unsigned read(unsigned count) {
        if (position_ > std::uint64_t(size_) * 8 || count > std::uint64_t(size_) * 8 - position_) {
            throw std::runtime_error("Truncated movie PCM bitstream");
        }
        unsigned value = 0;
        for (unsigned bit = 0; bit < count; ++bit, ++position_) {
            value |= ((bytes_.at(offset_ + position_ / 8) >> (position_ % 8)) & 1u) << bit;
        }
        return value;
    }

    unsigned bit() {
        instructions_ += 4 + (position_ % 32 == 0 ? 3 : 0);
        return read(1);
    }

    void byte() {
        const auto phase = position_ % 32;
        instructions_ += phase != 0 && phase <= 24 ? 7 : 15;
        read(8);
    }

    int node(Tree& nodes, unsigned depth) {
        if (nodes.size() >= 511 || depth > 256) { throw std::runtime_error("Movie PCM tree exceeds capacity"); }
        const auto index = static_cast<int>(nodes.size());
        nodes.push_back({});
        if (bit() != 0) {
            instructions_ += 11;
            nodes[index].left = node(nodes,depth + 1);
            nodes[index].right = node(nodes,depth + 1);
        } else {
            instructions_ += 9;
            byte();
        }
        return index;
    }

    Tree tree() {
        if (bit() != 1) { throw std::runtime_error("Missing movie PCM tree"); }
        instructions_ += 3;
        Tree nodes;
        node(nodes,0);
        if (bit() != 0) { throw std::runtime_error("Invalid movie PCM tree terminator"); }
        instructions_ += 4;
        return nodes;
    }

    void symbol(const Tree& nodes) {
        instructions_ += 4;
        int index = 0;
        while (nodes.at(index).left >= 0) {
            const auto direction = bit();
            instructions_ += 4 + (direction == 0);
            index = direction == 0 ? nodes[index].left : nodes[index].right;
        }
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_, size_;
    std::uint32_t decoded_bytes_;
    std::uint64_t position_ = 0, instructions_ = 0;
};

inline std::uint64_t pcm_work(const std::vector<std::uint8_t>& bytes, std::size_t frame) {
    const auto word = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) { value |= std::uint32_t(bytes.at(offset + byte)) << (byte * 8); }
        return value;
    };
    constexpr unsigned stereo_dpcm8 = 0xd0000000, audio_format_mask = 0xf0000000;
    if ((word(72) & audio_format_mask) != stereo_dpcm8) { return 0; }
    constexpr unsigned header_bytes = 104;
    const auto frames = std::uint64_t(word(12)) + (word(20) & 1);
    if (frame >= frames) { throw std::runtime_error("Movie PCM frame exceeds index"); }
    const auto types = header_bytes + frames * 4;
    auto offset = header_bytes + frames * 5 + word(52);
    for (std::size_t index = 0; index < frame; ++index) {
        const auto size = word(header_bytes + index * 4) & ~3u;
        if (offset > bytes.size() || size > bytes.size() - offset) { throw std::runtime_error("Truncated movie PCM frame"); }
        offset += size;
    }
    const auto frame_bytes = word(header_bytes + frame * 4) & ~3u;
    if (frames == 0 || offset > bytes.size() || frame_bytes > bytes.size() - offset) {
        throw std::runtime_error("Truncated movie PCM frame");
    }
    const auto end = offset + frame_bytes;
    const auto type = bytes.at(static_cast<std::size_t>(types + frame));
    if ((type & 2) == 0) { return 0; }
    if ((type & 1) != 0) { offset += std::uint64_t(bytes.at(static_cast<std::size_t>(offset))) * 4; }
    if (offset > end || end - offset < 8) { throw std::runtime_error("Truncated movie PCM packet"); }
    const auto packet = word(static_cast<std::size_t>(offset));
    const auto decoded = word(static_cast<std::size_t>(offset + 4));
    if (packet < 8 || packet > end - offset || decoded > word(24)) {
        throw std::runtime_error("Invalid movie PCM packet");
    }
    if (decoded == 0) { return 0; }
    return StereoPcm(bytes,static_cast<std::size_t>(offset + 8),packet - 8,decoded).work();
}

inline std::uint64_t first_pcm_work(const std::vector<std::uint8_t>& bytes) { return pcm_work(bytes,0); }

}
}
