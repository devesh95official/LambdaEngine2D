#include <sim/save.h>

#include <lam/core/data.h>
#include <sim/rules.h>
#include <sim/run.h>

namespace rl {
lam::Result<lam::SaveEnvelope> save_envelope(const lam::World &w,
                                             const lam::SerializeContext &ctx) {
    const RunState *run = run_state(w);
    if (!run)
        return lam::make_error(lam::ErrorCode::Invalid, "save_envelope: the world holds no run");
    lam::DataWriter world_out;
    LAM_TRY(lam::serialize_world(w, world_out, ctx)); // exact layout + snapshot resources
    LAM_TRY_ASSIGN(std::string world_json, world_out.finish());
    lam::DataWriter summary; // shown by slot lists
    summary.begin_object()
        .key("depth")
        .value(run->depth)
        .key("score")
        .value(run->score)
        .key("run_seed")
        .value(run->run_seed)
        .end_object();
    LAM_TRY_ASSIGN(std::string summary_json, summary.finish());
    return lam::SaveEnvelope{.game = std::string(kSaveGame),
                             .version = kSaveVersion,
                             .summary_json = std::move(summary_json),
                             .data_json = std::move(world_json)};
}

lam::Status load_envelope(lam::World &w, const lam::SaveEnvelope &env,
                          const lam::SerializeContext &ctx) {
    LAM_TRY_ASSIGN(lam::DataDocument doc, lam::DataDocument::parse(env.data_json, "save.data"));
    lam::DataReader root = doc.root();
    LAM_TRY(lam::deserialize_world(w, root, ctx)); // restore hooks: TileWorld, ChaseMap
    say(w, "Game loaded.", lam::Color{160, 200, 255, 255});
    return {};
}

lam::Result<std::uint64_t> saved_run_seed(const lam::SaveEnvelope &env) {
    return lam::parse_data<std::uint64_t>(env.summary_json, "save.summary",
                                          [](lam::DataReader &root) {
                                              root.ignore_unknown(); // depth, score: for slot lists
                                              return root.u64("run_seed");
                                          });
}

lam::Result<std::string> save_run(const lam::World &w, const lam::SerializeContext &ctx) {
    LAM_TRY_ASSIGN(lam::SaveEnvelope env, save_envelope(w, ctx));
    return lam::write_save(env);
}

lam::Status load_run(lam::World &w, std::string_view text, const lam::SerializeContext &ctx) {
    LAM_TRY_ASSIGN(lam::SaveEnvelope env, lam::read_save(text, kSaveGame, kSaveVersion, {}));
    return load_envelope(w, env, ctx);
}
} // namespace rl
