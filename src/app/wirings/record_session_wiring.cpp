// 录制接线器的实现(会话终章):/record 的会话件自 interactive_session
// 大类搬来(RecordCommandContext 的装配与录制器状态),行为一字未改。
#include "app/wirings/record_session_wiring.hpp"

#include <utility>

namespace lubancode::app {

RecordSessionWiring::RecordSessionWiring(Host host) : host_(std::move(host)) {}

lubancode::cli::RecordCommandContext RecordSessionWiring::MakeCommandContext() {
    lubancode::cli::RecordCommandContext record_ctx{recorder_,
                                                    host_.recordings_root != nullptr ? *host_.recordings_root
                                                                                     : std::filesystem::path(),
                                                    host_.project_skills_root != nullptr
                                                        ? *host_.project_skills_root
                                                        : std::filesystem::path(),
                                                    host_.global_skills_root != nullptr
                                                        ? *host_.global_skills_root
                                                        : std::filesystem::path(),
                                                    host_.refresh_skills};
    // P0-2 轨迹:选段器在位,/record 生命周期动作全走 selection。
    record_ctx.selection =
        host_.trajectory != nullptr ? &host_.trajectory->record_selection() : nullptr;
    return record_ctx;
}

}  // namespace lubancode::app
