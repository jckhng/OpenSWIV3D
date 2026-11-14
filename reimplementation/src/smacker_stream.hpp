#pragma once

#include "smacker_loading.hpp"

namespace swiv3d::detail {

struct SmackerStreamState {
    std::uint64_t position = 0, cached = 0, capacity = 0;
    std::uint64_t read = 0, write = 0, end = 0;
    std::uint64_t background_bytes = 0, high_water = 0;
};

class SmackerStream {
public:
    explicit SmackerStream(const std::vector<std::uint8_t>& bytes) : size_(bytes.size()) {
        const auto word = [&](std::size_t offset) {
            std::uint32_t value = 0;
            for (unsigned byte = 0; byte < 4; ++byte) { value |= std::uint32_t(bytes.at(offset + byte)) << (byte * 8); }
            return value;
        };
        const auto count = std::uint64_t(word(12)) + (word(20) & 1);
        const auto data = header_bytes + count * 5 + word(52);
        if (count == 0 || data > size_) { throw std::runtime_error("Truncated streaming movie index"); }
        std::uint64_t total = 0;
        for (std::size_t frame = 0; frame < count; ++frame) {
            const auto size = word(header_bytes + frame * 4);
            sizes_.push_back(size & ~3u);
            total += size;
        }
        const auto encoded = static_cast<std::int32_t>(word(16));
        const auto duration = encoded < 0 ? -std::int64_t(encoded) : std::int64_t(encoded) * 100;
        if (duration == 0) { throw std::runtime_error("Invalid streaming movie duration"); }
        const auto window = std::max<std::uint64_t>(1,100000 / duration);
        std::uint64_t best = total;
        if (count > window) {
            best = 0;
            std::uint64_t rolling = 0;
            for (std::size_t index = 0; index < window; ++index) { rolling += word(header_bytes + index * 4); }
            for (std::size_t index = 0; index < count - window; ++index) {
                if (index != 0) { best = std::max(best,rolling); }
                rolling = rolling - word(header_bytes + index * 4) + word(header_bytes + (index + window) * 4);
            }
        }
        const auto limit = std::max(3 * block_bytes,total / 4);
        const auto cache = std::clamp<std::uint64_t>(best,3 * block_bytes,limit);
        state_.capacity = (cache + block_bytes - 1) & ~(block_bytes - 1);
        enabled_ = state_.capacity < total * 4 / 5;
        if (!enabled_) { return; }
        state_.end = state_.capacity;
        state_.position = data;
        if (data % block_bytes != 0) {
            state_.cached = std::min(block_bytes - data % block_bytes,size_ - data);
            state_.write = state_.cached;
            state_.position += state_.cached;
        }
        read_frame(sizes_.front());
    }

    const SmackerStreamState& state() const { return state_; }

    MoviePreparation next() {
        if (!enabled_ || frame_ + 1 == sizes_.size()) { return {}; }
        return read_frame(sizes_[++frame_]);
    }

    MoviePreparation background() {
        MoviePreparation result;
        if (!enabled_) { return result; }
        if (frame_ >= sizes_.size()) { result.cpu(22); return result; }
        if (state_.capacity - state_.cached < block_bytes) { result.cpu(26); return result; }
        const auto returned = std::min(block_bytes,size_ - state_.position);
        const auto wrapped = state_.write + returned > state_.end;
        const auto high_water = state_.cached + returned > state_.high_water;
        // Normal streamed backgroundload path, including its two PIT reads.
        // Returned-byte branches in _radread precede the cache bookkeeping.
        constexpr unsigned before_read = 253, after_empty = 258;
        result.cpu(before_read);
        transfer(result,block_bytes,returned);
        const auto read_return = returned == block_bytes ? 5 : 3 * (returned != 0);
        result.cpu(after_empty + read_return +
                   (wrapped ? 3 : 0) + high_water);
        state_.position += returned;
        state_.cached += returned;
        state_.write += returned;
        if (wrapped) { state_.end = state_.write; state_.write = 0; }
        state_.background_bytes += returned;
        state_.high_water = std::max(state_.high_water,state_.cached);
        return result;
    }

private:
    static void transfer(MoviePreparation& result, std::uint64_t requested, std::uint64_t returned) {
        bool first = true;
        do {
            const auto request = std::min<std::uint64_t>(requested,8192);
            const auto actual = std::min(returned,request);
            const auto before = (first ? 902u : 585u) + (requested > 8192 ? 87u : 0u);
            const auto bridge = (first ? 1407u : 1090u) + smacker_loading::repeated(actual / 2) + actual % 2 +
                2 * (actual != request);
            result.cpu(before);
            result.disk(smacker_loading::disk_wait_work({static_cast<std::uint32_t>(actual)}));
            result.cpu(bridge - before);
            requested -= request;
            returned -= actual;
            first = false;
            if (actual != request) { break; }
        } while (requested != 0);
    }

    std::uint64_t read(MoviePreparation& result, std::uint64_t requested) {
        const auto actual = std::min(requested,size_ - state_.position);
        result.cpu(16);
        transfer(result,requested,actual);
        constexpr unsigned complete_read_return = 20, short_read_return = 17, empty_read_return = 15;
        result.cpu(actual == requested ? complete_read_return : actual != 0 ? short_read_return : empty_read_return);
        state_.position += actual;
        return actual;
    }

    MoviePreparation read_frame(std::uint64_t remaining) {
        MoviePreparation result;
        constexpr unsigned timer_read_work = 207;
        constexpr unsigned read_entry = 16, read_return = 20;
        result.cpu(read_entry + timer_read_work);
        while (remaining != 0) {
            result.cpu(2);
            if (state_.cached != 0) {
                const auto extent = state_.end - state_.read;
                const auto copied = std::min({remaining,state_.cached,state_.end - state_.read});
                result.cpu(42 + (extent > remaining) + (state_.cached < std::min(remaining,extent)));
                result.cpu(smacker_loading::repeated(copied / 4) + smacker_loading::repeated(copied % 4));
                state_.cached -= copied;
                state_.read += copied;
                remaining -= copied;
                if (state_.read >= state_.end) {
                    result.cpu(8);
                    state_.read = 0;
                    state_.end = state_.capacity;
                    if (state_.cached == 0) { state_.write = 0; result.cpu(2); }
                }
                if (remaining == 0) { result.cpu(8); }
                continue;
            }
            // The sound-service call remains a separate work contract. These
            // stages include its caller and the normal nonzero-PIT clock path.
            result.cpu(6);
            while (remaining > block_bytes) {
                result.cpu(16 + (remaining <= 4 * block_bytes));
                const auto requested = std::min(remaining & ~(block_bytes - 1),4 * block_bytes);
                if (read(result,requested) != requested) { throw std::runtime_error("Truncated streaming movie frame"); }
                remaining -= requested;
                result.cpu(23);
            }
            result.cpu(2);
            if (remaining != 0) {
                result.cpu(16);
                state_.cached = read(result,block_bytes);
                state_.read = 0;
                state_.write = state_.cached;
                if (state_.cached < remaining) { throw std::runtime_error("Truncated streaming movie tail"); }
                result.cpu(11);
            }
        }
        result.cpu(read_return + timer_read_work);
        return result;
    }

    static constexpr std::uint64_t block_bytes = 4096;
    static constexpr unsigned header_bytes = 104;
    std::uint64_t size_;
    std::vector<std::uint32_t> sizes_;
    std::size_t frame_ = 0;
    SmackerStreamState state_;
    bool enabled_ = false;
};

}
