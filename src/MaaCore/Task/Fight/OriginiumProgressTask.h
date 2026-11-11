#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Task/AbstractTask.h"

namespace asst
{
class MedicineCounterTaskPlugin;
class ProcessTask;

class OriginiumProgressTask final : public AbstractTask
{
public:
    OriginiumProgressTask(const AsstCallback& callback, Assistant* inst, std::string_view task_chain);
    virtual ~OriginiumProgressTask() noexcept override = default;

    bool set_params(
        std::string start_stage,
        int medicine,
        int medicine_expire_days,
        int stone,
        int max_normal_stages,
        bool include_raid,
        int last_chapter);

protected:
    virtual bool _run() override;

private:
    struct StageEntry
    {
        std::string code;
        std::string order_key;
        int chapter = 0;
        int category = 0;
        bool hard_only = false;
        bool story_only = false;
    };

    enum class Difficulty
    {
        Normal,
        Raid,
    };

    std::vector<StageEntry> build_stage_list() const;
    bool run_stage(const StageEntry& stage, Difficulty difficulty, bool& fought);
    bool navigate(const StageEntry& stage, Difficulty difficulty);
    bool switch_to_raid() const;
    bool has_unclaimed_originium() const;
    bool start_and_wait_for_map();
    bool is_stage_map(const cv::Mat& image) const;
    bool close_stage_detail() const;
    void report(std::string what, const StageEntry& stage, Difficulty difficulty);

    std::string m_start_stage = "0-1";
    int m_max_normal_stages = 0;
    bool m_include_raid = true;
    int m_last_chapter = 17;

    std::shared_ptr<ProcessTask> m_start_task_ptr;
    std::shared_ptr<MedicineCounterTaskPlugin> m_medicine_plugin_ptr;
};
} // namespace asst
