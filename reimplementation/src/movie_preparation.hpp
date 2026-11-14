#pragma once

#include "raster_schedule.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include <vector>

namespace swiv3d::detail {

enum class PreparationKind { cpu, disk };

struct PreparationStage {
    PreparationKind kind;
    std::uint64_t work;
};

class MoviePreparation {
public:
    void cpu(std::uint64_t work) { append(PreparationKind::cpu,work); }
    void disk(std::uint64_t work) { append(PreparationKind::disk,work); }
    std::uint64_t remaining() const { return remaining_; }
    const std::vector<PreparationStage>& stages() const { return stages_; }

    void reset(MoviePreparation next) {
        stages_ = std::move(next.stages_);
        remaining_ = next.remaining_;
        stage_ = 0;
        disk_started_ = false;
    }

    void elapse(std::uint64_t wall, std::uint64_t cpu) {
        consume(wall,cpu,{});
    }

    void elapse_device(std::uint64_t wall, std::uint64_t cpu, std::uint64_t begin,
                       const std::function<std::uint64_t(std::uint64_t)>& irq_work) {
        if (!disk_schedule_) { disk_schedule_.emplace(RasterPhase{},std::vector<CpuServiceWindow>{},IrqScheduling::device,irq_work); }
        consume(wall,cpu,begin);
    }

private:
    void consume(std::uint64_t wall, std::uint64_t cpu, std::optional<std::uint64_t> begin) {
        if (cpu > wall) { throw std::runtime_error("CPU work exceeds elapsed device time"); }
        const auto elapsed = wall;
        while (stage_ < stages_.size() && wall != 0) {
            auto& stage = stages_[stage_];
            if (stage.kind == PreparationKind::cpu) {
                // IRQ debt occupies the start of the device sample. Disk waits
                // overlap that service, while guest instructions resume after it.
                wall = cpu;
            } else if (begin && !disk_started_) {
                const auto start = *begin + elapsed - wall;
                const auto duration = disk_schedule_->idle(start,stage.work) - start;
                remaining_ += duration - stage.work;
                stage.work = duration;
                disk_started_ = true;
            }
            const auto used = std::min(stage.work,wall);
            stage.work -= used;
            remaining_ -= used;
            wall -= used;
            cpu = std::min(cpu,wall);
            if (stage.work != 0) { return; }
            ++stage_;
            disk_started_ = false;
        }
    }

    void append(PreparationKind kind, std::uint64_t work) {
        if (work == 0 && kind == PreparationKind::cpu) { return; }
        stages_.push_back({kind,work});
        remaining_ += work;
    }

    std::vector<PreparationStage> stages_;
    std::size_t stage_ = 0;
    std::uint64_t remaining_ = 0;
    std::optional<RasterSchedule> disk_schedule_;
    bool disk_started_ = false;
};

}
