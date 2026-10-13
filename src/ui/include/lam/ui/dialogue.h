#pragma once
// Branching dialogue: typewriter text, pages and choices.
// The runner copies its script, so the asset can reload mid-conversation.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/fixed_string.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
namespace lam {
struct DialogueChoice { FixedString<48> text; StrId next; };    // next none = end
// One page of dialogue: who speaks, what (UTF-8) and where it goes next.
struct DialogueLine {
    StrId id;                                                   // unique in its script
    StrId speaker;                                              // none = narration; id_name() to draw
    std::string text;
    std::array<DialogueChoice, 4> choices{};                    // the first choice_count are used
    std::uint8_t choice_count = 0;
    StrId next;                                                 // none = end
};
struct DialogueScript { std::vector<DialogueLine> lines; };     // asset ".dialogue.json"
// Plays one conversation. Presentation state; choices that change the game go through
// InputSystem::queue_intent.
class DialogueRunner {
public:
    // Copies `script` and shows `line` (none = the first). Unknown ids leave it inactive.
    void start(const DialogueScript &script, StrId line);
    void update(float dt, float chars_per_second = 40);         // <= 0 cps: whole line at once
    bool advance();                                             // finish typing, else next line; false at end
    // Picks a choice once the line is typed; false if out of range, still typing or no choices.
    bool choose(int index);
    bool active() const;                                        // a line is showing
    const DialogueLine *line() const;                           // null when inactive
    int visible_chars() const;                                  // UTF-8 code points (the shared decoder)
    bool typing_done() const;                                   // the whole line is visible (false inactive)

    std::string_view visible_text() const;                      // the typed prefix of line()->text
    int last_choice() const;                                    // index picked by choose(); -1 none
    int typed_last_update() const;                              // code points revealed by the last update (blips)
    void stop();                                                // ends the conversation
private:
    bool go_to(StrId id);
    DialogueScript script_;
    int current_ = -1;
    int length_ = 0;                                            // code points of the current text
    float typed_ = 0;
    int last_choice_ = -1;
    int typed_last_ = 0;
};
// Parses a .dialogue.json document (strict). "next" defaults to the following line unless the line
// has choices or "end": true. At most 4 choices per line.
Result<DialogueScript> parse_dialogue(std::string_view json, std::string_view file_name);
template <class T> class AssetLoader;                           // <lam/assets/asset_server.h>
std::unique_ptr<AssetLoader<DialogueScript>> make_dialogue_loader();   // ".dialogue.json"
}
