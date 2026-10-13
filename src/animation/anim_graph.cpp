// .anim.json graphs. Our own format, so parsing is strict. Names are interned at load so saves
// and logs can print them.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/animation/animator.h>
#include <lam/core/data.h>

namespace lam {
namespace {

std::string name_of(StrId id) {
    return id_display(id);
}

AnimClip read_clip(DataReader &c) {
    AnimClip clip;
    const std::string name = c.string("name");
    clip.name = intern(name);
    const bool has_tags = c.has("tags");
    const bool has_tag = c.has("tag");
    if (has_tags && has_tag) {
        c.fail("give either \"tag\" or \"tags\", not both");
        return clip;
    }
    if (has_tags) {
        DataArray tags = c.array("tags");
        const std::size_t n = tags.size();
        if (c.failed()) return clip;
        if (n != 1 && n != 2 && n != 4 && n != 8) {
            tags.fail("must list 1, 2, 4 or 8 tags (one per facing), not " + std::to_string(n));
            return clip;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::string t = tags.text(i);
            if (!t.empty()) clip.tags[i] = intern(t);
        }
        clip.dir_count = static_cast<std::uint8_t>(n);
    } else if (has_tag) {
        clip.tags[0] = intern(c.string("tag"));
    } else {
        clip.tags[0] = clip.name; // a clip named after its tag
    }
    if (!c.failed() && !clip.tags[0]) c.fail("the first facing (right) needs a tag");
    clip.mirror_left = c.boolean_or("mirror_left", true);
    return clip;
}

std::uint8_t clip_index(DataReader &r, const AnimGraph &g, std::string_view key, bool allow_any) {
    const std::string name = r.string(key);
    if (r.failed()) return 0;
    if (allow_any && (name == "*" || name == "any")) return kAnyClip;
    const int i = g.find_clip(str_id(name));
    if (i < 0) {
        r.fail(std::string(key) + " \"" + name + "\" is not a clip of this graph");
        return 0;
    }
    return static_cast<std::uint8_t>(i);
}

AnimTransition read_transition(DataReader &t, const AnimGraph &g) {
    AnimTransition tr;
    tr.from = clip_index(t, g, "from", true);
    tr.to = clip_index(t, g, "to", false);
    if (t.has("param")) tr.param = intern(t.string("param"));
    const bool above = t.has("above");
    const bool below = t.has("below");
    const bool threshold = t.has("threshold");
    if ((above ? 1 : 0) + (below ? 1 : 0) + (threshold ? 1 : 0) > 1) {
        t.fail("give one of \"above\", \"below\" or \"threshold\"");
        return tr;
    }
    if ((above || below || threshold || t.has("greater")) && !tr.param) {
        t.fail("a threshold needs a \"param\"");
        return tr;
    }
    if (above) {
        tr.threshold = t.number("above", -1e30f, 1e30f);
        tr.greater = true;
    } else if (below) {
        tr.threshold = t.number("below", -1e30f, 1e30f);
        tr.greater = false;
    } else {
        tr.threshold = t.number_or("threshold", 0.5f, -1e30f, 1e30f);
    }
    if (!above && !below) tr.greater = t.boolean_or("greater", true);
    tr.on_finish = t.boolean_or("on_finish", false);
    if (t.has("trigger")) tr.trigger = intern(t.string("trigger"));
    if (!t.failed() && tr.from == kAnyClip && !tr.param && !tr.trigger && !tr.on_finish) {
        t.fail("a transition from any clip needs a param, a trigger or on_finish "
               "(it would fire every tick)");
    }
    return tr;
}

AnimGraph read_graph(DataReader &in) {
    AnimGraph g;
    if (in.has("sheet")) g.sheet_path = in.string("sheet");
    DataArray clips = in.array("clips");
    if (in.failed()) return g;
    if (clips.size() == 0) {
        clips.fail("must not be empty");
        return g;
    }
    if (clips.size() > 255) {
        clips.fail("at most 255 clips");
        return g;
    }
    g.clips.reserve(clips.size());
    for (std::size_t i = 0; i < clips.size() && !in.failed(); ++i) {
        DataReader c = clips.object(i);
        AnimClip clip = read_clip(c);
        if (in.failed()) break;
        if (g.find_clip(clip.name) >= 0) {
            c.fail("duplicate clip name \"" + name_of(clip.name) + "\"");
            break;
        }
        g.clips.push_back(clip);
    }
    if (in.failed()) return g;
    if (in.has("start")) {
        g.start = clip_index(in, g, "start", false);
        if (in.failed()) return g;
    }
    if (in.has("transitions")) {
        DataArray trs = in.array("transitions");
        for (std::size_t i = 0; i < trs.size() && !in.failed(); ++i) {
            DataReader t = trs.object(i);
            AnimTransition tr = read_transition(t, g);
            if (!in.failed()) g.transitions.push_back(tr);
        }
    }
    if (in.has("events")) {
        DataArray evs = in.array("events");
        for (std::size_t i = 0; i < evs.size() && !in.failed(); ++i) {
            DataReader e = evs.object(i);
            FrameEvent ev;
            ev.tag = intern(e.string("tag"));
            ev.frame = static_cast<std::uint16_t>(e.integer("frame", 0, 65535));
            ev.name = intern(e.string("name"));
            if (!in.failed()) g.frame_events.push_back(ev);
        }
    }
    return g;
}

} // namespace

Result<AnimGraph> parse_anim_graph(std::string_view text, std::string_view file_name) {
    return parse_data<AnimGraph>(text, file_name, [](DataReader &in) { return read_graph(in); });
}

Status check_anim_graph(const AnimGraph &g, const SpriteSheet &s) {
    if (g.clips.empty()) return make_error(ErrorCode::Invalid, "animation graph has no clips");
    if (g.start >= g.clips.size())
        return make_error(ErrorCode::Invalid, "animation graph start clip is out of range");
    for (const AnimClip &c : g.clips) {
        const std::uint8_t n = c.dir_count >= 8 ? 8 : c.dir_count >= 4 ? 4 : c.dir_count >= 2 ? 2 : 1;
        if (c.dir_count != n) {
            return make_error(ErrorCode::Invalid, "clip \"" + name_of(c.name) +
                                                      "\": dir_count must be 1, 2, 4 or 8");
        }
        for (std::uint8_t k = 0; k < n; ++k) {
            if (c.tags[k] && s.find_tag(c.tags[k]) < 0) {
                return make_error(ErrorCode::NotFound, "clip \"" + name_of(c.name) + "\": tag \"" +
                                                           name_of(c.tags[k]) +
                                                           "\" is not in the sheet");
            }
            bool flip = false;
            const StrId t = clip_tag(c, k, n, &flip);
            if (!t || s.find_tag(t) < 0) {
                return make_error(ErrorCode::Invalid, "clip \"" + name_of(c.name) + "\": facing " +
                                                          std::to_string(k) + " has no tag");
            }
        }
    }
    for (const AnimTransition &tr : g.transitions) {
        if ((tr.from != kAnyClip && tr.from >= g.clips.size()) || tr.to >= g.clips.size())
            return make_error(ErrorCode::Invalid, "transition refers to a clip out of range");
    }
    for (const FrameEvent &ev : g.frame_events) {
        int length = -1; // frames in the tag (or the longest tag of the clip)
        if (const int t = s.find_tag(ev.tag); t >= 0) {
            const SheetTag &tag = s.tags[static_cast<std::size_t>(t)];
            length = tag.to - tag.from + 1;
        } else if (const int c = g.find_clip(ev.tag); c >= 0) {
            const AnimClip &clip = g.clips[static_cast<std::size_t>(c)];
            for (std::uint8_t k = 0; k < clip.dir_count && k < clip.tags.size(); ++k) {
                if (const int ct = s.find_tag(clip.tags[k]); ct >= 0) {
                    const SheetTag &tag = s.tags[static_cast<std::size_t>(ct)];
                    length = std::max(length, tag.to - tag.from + 1);
                }
            }
        } else {
            return make_error(ErrorCode::NotFound, "event \"" + name_of(ev.name) + "\": \"" +
                                                       name_of(ev.tag) +
                                                       "\" is neither a sheet tag nor a clip");
        }
        if (static_cast<int>(ev.frame) >= length) {
            return make_error(ErrorCode::OutOfRange,
                              "event \"" + name_of(ev.name) + "\": frame " +
                                  std::to_string(ev.frame) + " is past the end of \"" +
                                  name_of(ev.tag) + "\" (" + std::to_string(length) + " frames)");
        }
    }
    return {};
}

} // namespace lam
