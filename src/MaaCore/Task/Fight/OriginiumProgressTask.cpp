#include "OriginiumProgressTask.h"

#include <algorithm>
#include <array>
#include <climits>
#include <map>
#include <set>
#include <tuple>

#include <boost/regex.hpp>

#include "Config/Miscellaneous/StageDropsConfig.h"
#include "Controller/Controller.h"
#include "Task/Fight/MedicineCounterTaskPlugin.h"
#include "Task/Fight/StageNavigationTask.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/OCRer.h"

asst::OriginiumProgressTask::OriginiumProgressTask(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain) :
    AbstractTask(callback, inst, task_chain),
    m_start_task_ptr(std::make_shared<ProcessTask>(m_callback, m_inst, task_chain))
{
    m_start_task_ptr->set_tasks({ "OriginiumFightBegin" })
        .set_times_limit("OriginiumStartButton1", INT_MAX)
        .set_times_limit("OriginiumStartButton2", INT_MAX)
        .set_times_limit("OriginiumStartStory", INT_MAX);
    m_medicine_plugin_ptr = m_start_task_ptr->register_plugin<MedicineCounterTaskPlugin>();
}

bool asst::OriginiumProgressTask::set_params(
    std::string start_stage,
    int medicine,
    int medicine_expire_days,
    int stone,
    int max_normal_stages,
    bool include_raid,
    int last_chapter)
{
    if (medicine < 0 || medicine_expire_days < 0 || stone < 0 || max_normal_stages < 0 || last_chapter < 0) {
        LogError << __FUNCTION__ << "invalid limits" << medicine << medicine_expire_days << stone << max_normal_stages
                 << last_chapter;
        return false;
    }

    if (start_stage.empty()) {
        start_stage = "0-1";
    }

    m_start_stage = std::move(start_stage);
    m_max_normal_stages = max_normal_stages;
    m_include_raid = include_raid;
    m_last_chapter = last_chapter;
    m_start_task_ptr->set_times_limit("OriginiumStoneConfirm", stone);
    m_medicine_plugin_ptr->set_count(medicine);
    m_medicine_plugin_ptr->set_expire_days(medicine_expire_days);
    m_medicine_plugin_ptr->set_reduce_when_exceed(true);
    return true;
}

bool asst::OriginiumProgressTask::_run()
{
    LogTraceFunction;

    m_resource_exhausted = false;

    const auto stages = build_stage_list();
    const auto start = std::ranges::find(stages, m_start_stage, &StageEntry::code);
    if (start == stages.end()) {
        LogError << __FUNCTION__ << "start stage is not a supported main-story stage" << m_start_stage;
        return false;
    }

    LogInfo << "Originium progression started from" << m_start_stage << "through chapter" << m_last_chapter
            << "include raid:" << m_include_raid << "normal stage limit:" << m_max_normal_stages;

    int normal_fought = 0;
    for (auto iter = start; iter != stages.end() && !need_exit(); ++iter) {
        if (iter->chapter > m_last_chapter) {
            break;
        }
        if (m_max_normal_stages > 0 && normal_fought >= m_max_normal_stages) {
            break;
        }

        bool fought = false;
        if (!run_stage(*iter, Difficulty::Normal, fought)) {
            return false;
        }
        if (m_resource_exhausted) {
            LogInfo << "Originium progression stopped: sanity recovery budget exhausted at" << iter->code;
            break;
        }
        normal_fought += fought ? 1 : 0;

        if (m_include_raid && !iter->hard_only && !iter->story_only) {
            bool raid_fought = false;
            if (!run_stage(*iter, Difficulty::Raid, raid_fought)) {
                return false;
            }
            if (m_resource_exhausted) {
                LogInfo << "Originium progression stopped: sanity recovery budget exhausted at raid" << iter->code;
                break;
            }
        }
    }

    return !need_exit();
}

std::vector<asst::OriginiumProgressTask::StageEntry> asst::OriginiumProgressTask::build_stage_list() const
{
    std::vector<StageEntry> stages;
    std::set<std::string> seen;
    std::map<int, int> max_numeric_stage;
    const auto numeric_order_key = [](int chapter, int index) {
        return std::string("main_") + (chapter < 10 ? "0" : "") + std::to_string(chapter) + "-" +
               (index < 10 ? "0" : "") + std::to_string(index);
    };
    static const boost::regex StageIdRegex(R"(^(main|sub)_(\d{2})-(.+)$)");

    for (const auto& code : StageDrops.get_all_stage_code()) {
        const auto& info = StageDrops.get_stage_info(code, StageDifficulty::Normal);
        boost::smatch match;
        if (!boost::regex_match(info.stage_id, match, StageIdRegex)) {
            continue;
        }

        const int chapter = std::stoi(match[2].str());
        if (chapter > m_last_chapter || !seen.emplace(code).second) {
            continue;
        }
        stages.emplace_back(
            StageEntry {
                .code = code,
                .order_key = info.stage_id,
                .chapter = chapter,
                .category = match[1].str() == "main" ? 0 : 1,
                .hard_only = false,
                .story_only = false,
            });

        boost::smatch code_match;
        static const boost::regex NumericCodeRegex(R"(^(\d+)-(\d+)$)");
        if (boost::regex_match(code, code_match, NumericCodeRegex) && std::stoi(code_match[1].str()) == chapter) {
            const int index = std::stoi(code_match[2].str());
            max_numeric_stage[chapter] = std::max(max_numeric_stage[chapter], index);
            stages.back().order_key = numeric_order_key(chapter, index);
        }
    }

    // stages.json only contains battle/drop stages.  Pure story nodes (for example 6-6)
    // are gaps in the displayed numeric sequence, but must be opened before later nodes unlock.
    for (const auto& [chapter, max_index] : max_numeric_stage) {
        for (int index = 1; index <= max_index; ++index) {
            const std::string code = std::to_string(chapter) + "-" + std::to_string(index);
            if (!seen.emplace(code).second) {
                continue;
            }
            stages.emplace_back(
                StageEntry {
                    .code = code,
                    .order_key = numeric_order_key(chapter, index),
                    .chapter = chapter,
                    .category = 0,
                    .hard_only = false,
                    .story_only = true,
                });
        }
    }

    // Episode 8 uses R8/M8/JT8 battle codes rather than numeric codes, so it
    // cannot participate in the numeric-gap inference above. END8-1 is the
    // story node that closes the episode and unlocks the following episode.
    if (m_last_chapter >= 8 && seen.emplace("END8-1").second) {
        stages.emplace_back(
            StageEntry {
                .code = "END8-1",
                .order_key = "main_08-99",
                .chapter = 8,
                .category = 0,
                .hard_only = false,
                .story_only = true,
            });
    }

    static constexpr std::array HardStageCounts = {
        std::pair { 5, 4 },  std::pair { 6, 4 },  std::pair { 7, 4 },  std::pair { 8, 4 },  std::pair { 9, 6 },
        std::pair { 10, 3 }, std::pair { 11, 4 }, std::pair { 12, 4 }, std::pair { 13, 4 }, std::pair { 14, 4 },
        std::pair { 15, 4 }, std::pair { 16, 4 }, std::pair { 17, 4 },
    };
    for (const auto& [chapter, count] : HardStageCounts) {
        if (chapter > m_last_chapter) {
            continue;
        }
        for (int index = 1; index <= count; ++index) {
            const std::string code = "H" + std::to_string(chapter) + "-" + std::to_string(index);
            stages.emplace_back(
                StageEntry {
                    .code = code,
                    .order_key = code,
                    .chapter = chapter,
                    .category = 2,
                    .hard_only = true,
                    .story_only = false,
                });
        }
    }

    std::ranges::sort(stages, [](const StageEntry& lhs, const StageEntry& rhs) {
        return std::tie(lhs.chapter, lhs.category, lhs.order_key) < std::tie(rhs.chapter, rhs.category, rhs.order_key);
    });
    return stages;
}

bool asst::OriginiumProgressTask::run_stage(const StageEntry& stage, Difficulty difficulty, bool& fought)
{
    fought = false;
    LogInfo << "Originium checking stage" << stage.code
            << "difficulty:" << (difficulty == Difficulty::Raid ? "raid" : "normal")
            << "story only:" << stage.story_only;
    if (!navigate(stage, difficulty)) {
        if (difficulty == Difficulty::Raid || stage.story_only) {
            report(stage.story_only ? "OriginiumStoryUnavailable" : "OriginiumRaidUnavailable", stage, difficulty);
            return true;
        }
        LogError << __FUNCTION__ << "failed to navigate to required normal stage" << stage.code;
        return false;
    }

    if (difficulty == Difficulty::Raid && stage.chapter < 10 && !switch_to_raid()) {
        report("OriginiumRaidUnavailable", stage, difficulty);
        close_stage_detail();
        return true;
    }

    if (stage.story_only && !has_start_story_button()) {
        report("OriginiumStoryAlreadyCompleted", stage, difficulty);
        return close_stage_detail();
    }

    if (!stage.story_only && !has_unclaimed_originium()) {
        report("OriginiumRewardAlreadyClaimed", stage, difficulty);
        return close_stage_detail();
    }

    report("OriginiumStageStart", stage, difficulty);
    const auto outcome = start_and_wait_for_map();
    if (outcome == StartOutcome::ResourceExhausted) {
        m_resource_exhausted = true;
        report("OriginiumSanityRecoveryExhausted", stage, difficulty);
        if (!close_stage_detail()) {
            LogWarn << __FUNCTION__ << "could not close stage detail after sanity recovery budget was exhausted";
        }
        return true;
    }
    if (outcome == StartOutcome::Failed) {
        LogError << __FUNCTION__ << "failed to start or settle stage" << stage.code;
        return false;
    }

    fought = true;
    report("OriginiumStageCompleted", stage, difficulty);
    return true;
}

bool asst::OriginiumProgressTask::navigate(const StageEntry& stage, Difficulty difficulty)
{
    std::string navigation_code = stage.code;
    if (stage.chapter >= 10) {
        navigation_code += stage.hard_only || difficulty == Difficulty::Raid ? "-Hard" : "-Normal";
    }

    StageNavigationTask navigation(m_callback, m_inst, m_task_chain);
    navigation.set_task_id(m_task_id);
    navigation.set_retry_times(0);
    if (!navigation.set_stage_name(navigation_code)) {
        return false;
    }
    return navigation.run();
}

bool asst::OriginiumProgressTask::switch_to_raid() const
{
    ProcessTask task(*this, { "ChangeToRaidDifficulty", "RaidConfirm" });
    task.set_retry_times(0);
    return task.run();
}

bool asst::OriginiumProgressTask::has_unclaimed_originium() const
{
    static constexpr std::array RewardMarkers = { "首次掉落", "首次通关", "至纯源石" };
    static constexpr std::array ClaimedMarkers = { "已获得", "已领取", "已完成" };

    OCRer analyzer(ctrler()->get_image());
    const auto results = analyzer.analyze();
    if (!results) {
        return false;
    }

    bool reward = false;
    bool claimed = false;
    for (const auto& result : *results) {
        reward = reward || std::ranges::any_of(RewardMarkers, [&](const char* marker) {
                     return result.text.find(marker) != std::string::npos;
                 });
        claimed = claimed || std::ranges::any_of(ClaimedMarkers, [&](const char* marker) {
                      return result.text.find(marker) != std::string::npos;
                  });
    }
    return reward && !claimed;
}

bool asst::OriginiumProgressTask::has_start_story_button() const
{
    OCRer analyzer(ctrler()->get_image());
    const auto results = analyzer.analyze();
    return results && std::ranges::any_of(*results, [](const auto& result) {
               return result.text.find("开始剧情") != std::string::npos;
           });
}

asst::OriginiumProgressTask::StartOutcome asst::OriginiumProgressTask::start_and_wait_for_map()
{
    m_start_task_ptr->set_task_id(m_task_id);
    if (!m_start_task_ptr->run()) {
        LogError << __FUNCTION__ << "start button process failed; last task:" << m_start_task_ptr->get_last_task_name();
        return StartOutcome::Failed;
    }
    const auto& last_task = m_start_task_ptr->get_last_task_name();
    if (last_task.ends_with("OriginiumCloseStonePage") || last_task.ends_with("OriginiumNoStone")) {
        return StartOutcome::ResourceExhausted;
    }
    if (!last_task.ends_with("OriginiumStartButton2") && !last_task.ends_with("OriginiumStartStory")) {
        LogError << __FUNCTION__ << "start process ended at unexpected task:" << last_task;
        return StartOutcome::Failed;
    }

    constexpr int MinWaitMs = 3000;
    constexpr int PollMs = 1000;
    constexpr int TimeoutMs = 180'000;
    sleep(MinWaitMs);

    int stable_frames = 0;
    bool story_skip_requested = false;
    for (int elapsed = MinWaitMs; elapsed < TimeoutMs && !need_exit(); elapsed += PollMs) {
        const auto image = ctrler()->get_image();
        bool stage_detail_open = false;
        OCRer story_ocr(image);
        if (const auto results = story_ocr.analyze()) {
            stage_detail_open = std::ranges::any_of(*results, [](const auto& result) {
                return result.text.find("开始行动") != std::string::npos ||
                       result.text.find("开始作战") != std::string::npos ||
                       result.text.find("开始推演") != std::string::npos ||
                       result.text.find("开始剧情") != std::string::npos;
            });
            const auto skip = std::ranges::find_if(*results, [](const auto& result) {
                return result.text.find("跳过") != std::string::npos || result.text.find("略过") != std::string::npos;
            });
            if (!story_skip_requested && skip != results->end()) {
                ctrler()->click(skip->rect);
                story_skip_requested = true;
                stable_frames = 0;
                sleep(PollMs);
                continue;
            }
            if (story_skip_requested) {
                const auto confirm = std::ranges::find_if(*results, [](const auto& result) {
                    const auto center_x = result.rect.x + result.rect.width / 2;
                    return center_x > 640 && (result.text.find("确认") != std::string::npos ||
                                              result.text.find("确定") != std::string::npos);
                });
                if (confirm != results->end()) {
                    ctrler()->click(confirm->rect);
                    story_skip_requested = true;
                    stable_frames = 0;
                    sleep(PollMs);
                    continue;
                }
            }
        }

        if (!stage_detail_open && is_stage_map(image)) {
            ++stable_frames;
            if (stable_frames >= 2) {
                return StartOutcome::Started;
            }
        }
        else {
            stable_frames = 0;
        }
        if (elapsed % 10'000 == 0) {
            LogInfo << "Originium waiting for automatic settlement; elapsed ms:" << elapsed;
        }
        sleep(PollMs);
    }
    LogError << __FUNCTION__ << "automatic settlement did not return to the stage map within ms:" << TimeoutMs;
    return StartOutcome::Failed;
}

bool asst::OriginiumProgressTask::is_stage_map(const cv::Mat& image) const
{
    static const boost::regex StageRegex(R"(^[A-Za-z]{0,3}\d{1,2}-\d{1,2}$)");

    OCRer analyzer(image);
    analyzer.set_task_info("OriginiumMapStageOcr");
    const auto results = analyzer.analyze();
    if (!results) {
        return false;
    }

    for (const auto& result : *results) {
        if (boost::regex_match(result.text, StageRegex)) {
            return true;
        }
    }
    return false;
}

bool asst::OriginiumProgressTask::close_stage_detail() const
{
    // A failed raid switch may leave a modal over the stage detail. The first
    // return closes that modal and the second one closes the detail itself.
    // Stop after one click in the usual case so we never back out of the map.
    for (int attempt = 0; attempt < 2; ++attempt) {
        ProcessTask task(*this, { "Return" });
        task.set_retry_times(3);
        if (!task.run()) {
            return false;
        }

        OCRer analyzer(ctrler()->get_image());
        const auto results = analyzer.analyze();
        const bool still_in_detail = results && std::ranges::any_of(*results, [](const auto& result) {
                                         return result.text.find("开始行动") != std::string::npos ||
                                                result.text.find("开始作战") != std::string::npos ||
                                                result.text.find("开始推演") != std::string::npos ||
                                                result.text.find("开始剧情") != std::string::npos;
                                     });
        if (!still_in_detail) {
            return true;
        }
    }
    LogError << __FUNCTION__ << "stage detail remained open after two return attempts";
    return false;
}

void asst::OriginiumProgressTask::report(std::string what, const StageEntry& stage, Difficulty difficulty)
{
    auto info = basic_info_with_what(std::move(what));
    info["details"]["stage"] = stage.code;
    info["details"]["difficulty"] = difficulty == Difficulty::Raid ? "raid" : "normal";
    callback(AsstMsg::SubTaskExtraInfo, info);
}
