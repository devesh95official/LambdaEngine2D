// Branching dialogue: runner, .dialogue.json parser and loader.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/data.h>
#include <lam/core/log.h>
#include <lam/core/text.h>
#include <lam/ui/dialogue.h>

namespace lam {

// ---- DialogueRunner ----------------------------------------------------------------------------

bool DialogueRunner::go_to(StrId id) {
    for (std::size_t i = 0; i < script_.lines.size(); ++i) {
        if (script_.lines[i].id == id) {
            current_ = static_cast<int>(i);
            length_ = utf8_length(script_.lines[i].text);
            typed_ = 0.0f;
            return true;
        }
    }
    current_ = -1;
    length_ = 0;
    typed_ = 0.0f;
    return false;
}

void DialogueRunner::start(const DialogueScript &script, StrId line) {
    if (&script != &script_) script_ = script;
    current_ = -1;
    length_ = 0;
    typed_ = 0.0f;
    last_choice_ = -1;
    typed_last_ = 0;
    if (script_.lines.empty()) {
        LAM_LOG_WARN("DialogueRunner::start: the script has no lines");
        return;
    }
    if (!line) line = script_.lines.front().id;
    if (!go_to(line)) LAM_LOG_WARN("DialogueRunner::start: no line '{}'", id_display(line));
}

void DialogueRunner::update(float dt, float chars_per_second) {
    typed_last_ = 0;
    if (!active() || typing_done()) return;
    const int before = visible_chars();
    if (!(chars_per_second > 0.0f) || !std::isfinite(chars_per_second)) {
        typed_ = static_cast<float>(length_);
    } else if (dt > 0.0f && std::isfinite(dt)) {
        typed_ = std::min(static_cast<float>(length_), typed_ + dt * chars_per_second);
    }
    typed_last_ = visible_chars() - before;
}

bool DialogueRunner::advance() {
    if (!active()) return false;
    if (!typing_done()) {
        typed_ = static_cast<float>(length_);
        typed_last_ = 0;
        return true;
    }
    const DialogueLine &l = script_.lines[static_cast<std::size_t>(current_)];
    if (l.choice_count > 0) return true;                        // waits for choose()
    if (!l.next) {
        current_ = -1;
        return false;
    }
    return go_to(l.next);
}

bool DialogueRunner::choose(int index) {
    if (!active() || !typing_done()) return false;
    const DialogueLine &l = script_.lines[static_cast<std::size_t>(current_)];
    if (index < 0 || index >= l.choice_count) return false;
    last_choice_ = index;
    const StrId next = l.choices[static_cast<std::size_t>(index)].next;
    if (!next) current_ = -1;
    else go_to(next);
    return true;
}

bool DialogueRunner::active() const {
    return current_ >= 0;
}

const DialogueLine *DialogueRunner::line() const {
    return active() ? &script_.lines[static_cast<std::size_t>(current_)] : nullptr;
}

int DialogueRunner::visible_chars() const {
    if (!active()) return 0;
    return std::min(static_cast<int>(typed_), length_);
}

bool DialogueRunner::typing_done() const {
    return active() && visible_chars() >= length_;
}

std::string_view DialogueRunner::visible_text() const {
    const DialogueLine *l = line();
    return l == nullptr ? std::string_view{} : utf8_prefix(l->text, visible_chars());
}

int DialogueRunner::last_choice() const {
    return last_choice_;
}

int DialogueRunner::typed_last_update() const {
    return typed_last_;
}

void DialogueRunner::stop() {
    current_ = -1;
}

// ---- parsing -----------------------------------------------------------------------------------

namespace {

struct ParsedLine {
    DialogueLine line;
    bool has_next = false;
    bool end = false;
};

DialogueScript read_script(DataReader &root) {
    DialogueScript script;
    DataArray lines = root.array("lines");
    if (lines.size() == 0) {
        root.fail("\"lines\" must hold at least one line");
        return script;
    }
    std::vector<ParsedLine> parsed;
    parsed.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        DataReader l = lines.object(i);
        ParsedLine p;
        p.line.id = l.has("id") ? l.id("id") : intern(std::format("line{}", i));
        if (l.has("speaker")) p.line.speaker = l.id("speaker");
        p.line.text = l.text("text");
        p.end = l.boolean_or("end", false);
        if (l.has("next")) {
            p.line.next = l.id("next");
            p.has_next = true;
            if (p.end) l.fail("a line cannot have both \"next\" and \"end\"");
        }
        if (l.has("choices")) {
            DataArray choices = l.array("choices");
            if (choices.size() > 4) {
                l.fail("at most 4 choices per line");
            } else if (choices.size() == 0) {
                l.fail("\"choices\" must not be empty");
            }
            const std::size_t n = std::min<std::size_t>(choices.size(), 4);
            for (std::size_t k = 0; k < n; ++k) {
                DataReader c = choices.object(k);
                const std::string text = c.string("text");
                if (text.size() > 47) c.fail("choice text is longer than 47 bytes");
                p.line.choices[k].text = text;
                if (c.has("next")) p.line.choices[k].next = c.id("next");
            }
            p.line.choice_count = static_cast<std::uint8_t>(n);
            if (p.has_next) l.fail("a line with choices cannot have \"next\" (each choice has its own)");
        }
        parsed.push_back(std::move(p));
    }
    // Default next: the following line.
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        ParsedLine &p = parsed[i];
        if (!p.has_next && !p.end && p.line.choice_count == 0 && i + 1 < parsed.size())
            p.line.next = parsed[i + 1].line.id;
    }
    // Ids unique; every target exists.
    const auto exists = [&parsed](StrId id) {
        return std::any_of(parsed.begin(), parsed.end(), [id](const ParsedLine &p) { return p.line.id == id; });
    };
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const DialogueLine &l = parsed[i].line;
        for (std::size_t j = 0; j < i; ++j) {
            if (parsed[j].line.id == l.id) {
                root.fail(std::format("lines/{}: duplicate id '{}'", i, id_display(l.id)));
                return script;
            }
        }
        if (l.next && !exists(l.next)) {
            root.fail(std::format("lines/{}/next: no line '{}'", i, id_display(l.next)));
            return script;
        }
        for (std::size_t k = 0; k < l.choice_count; ++k) {
            const StrId next = l.choices[k].next;
            if (next && !exists(next)) {
                root.fail(std::format("lines/{}/choices/{}/next: no line '{}'", i, k, id_display(next)));
                return script;
            }
        }
    }
    script.lines.reserve(parsed.size());
    for (ParsedLine &p : parsed) script.lines.push_back(std::move(p.line));
    return script;
}

class DialogueLoader final : public AssetLoader<DialogueScript> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExt[] = {".dialogue.json"};
        return kExt;
    }
    Result<DialogueScript> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return parse_dialogue(text, ctx.path());
    }
};

} // namespace

Result<DialogueScript> parse_dialogue(std::string_view json, std::string_view file_name) {
    return parse_data<DialogueScript>(json, file_name, [](DataReader &root) { return read_script(root); });
}

std::unique_ptr<AssetLoader<DialogueScript>> make_dialogue_loader() {
    return std::make_unique<DialogueLoader>();
}

} // namespace lam
