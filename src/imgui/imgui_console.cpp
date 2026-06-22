/**
 * imgui_console.cpp - In-game ImGui Lua console implementation.
 *
 * Output buffer is fed from any thread via imgui_console_add_log() (staged in a
 * mutex-guarded pending list, spliced into the render-thread-owned item list each
 * frame). Input is submitted via the registered callback. Modeled on Dear ImGui's
 * ExampleAppConsole, trimmed to a Lua REPL.
 */

#include "imgui_console.h"
#include "imgui.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct ConsoleItem {
    std::string text;
    bool is_error;
};

constexpr size_t kMaxItems = 5000;

std::mutex g_mutex;                       // guards g_pending / g_want_clear
std::vector<ConsoleItem> g_pending;      // cross-thread inbox
std::vector<ConsoleItem> g_items;        // render-thread-owned display list
bool g_want_clear = false;

std::vector<std::string> g_history;
int g_history_pos = -1;                   // -1 = new line (not browsing history)

char g_input[4096] = "";
bool g_scroll_to_bottom = false;
bool g_focus_input = false;

std::string g_pending_paste;      // clipboard text to insert into the input field
bool g_has_pending_paste = false;

imgui_console_submit_fn g_submit = nullptr;

// "Level Up" button: bank one more level per click for each active party
// member by granting the XP needed to reach the next cumulative-XP threshold
// ABOVE their current TotalExperience (standard BG3 table, capped at level 12).
// Keys off total XP, NOT the applied level, so repeated clicks stack pending
// level-ups even before they're applied in the character sheet. Per-character
// (XP is not always shared). Queued via the submit callback (Lua thread).
//
// Iterates eoc::party::MemberComponent entities DIRECTLY (handle-based) instead
// of GetPartyMembers()->Get(uuid): Ext.Entity.Get(uuid) can return a stale
// handle after an in-game save-load (the guid->handle cache isn't invalidated),
// which made component reads return nil. The XP grant still uses the UUID
// (read fresh off the entity) since Osi.AddExplorationExperience takes a GUID.
const char *kLevelUpLua =
    "local TH={300,900,2700,6500,14000,23000,34000,48000,64000,85000,100000} "
    "local n=0 "
    "for _,e in ipairs(Ext.Entity.GetAllEntitiesWithComponent('eoc::party::MemberComponent')) do "
    "  local x=e.Experience; local u=e.Uuid and e.Uuid.EntityUuid "
    "  if x and u then "
    "    local total=x.TotalExperience or 0; local target "
    "    for _,t in ipairs(TH) do if t>total then target=t break end end "
    "    if target then Osi.AddExplorationExperience(u,target-total); n=n+1 end "
    "  end "
    "end "
    "_P('Level Up: banked one level for '..n..' party member(s) "
    "(apply them in each character sheet)') ";

int input_text_callback(ImGuiInputTextCallbackData *data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const int prev = g_history_pos;
        if (data->EventKey == ImGuiKey_UpArrow) {
            if (g_history_pos == -1) {
                g_history_pos = (int)g_history.size() - 1;
            } else if (g_history_pos > 0) {
                g_history_pos--;
            }
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (g_history_pos != -1) {
                if (++g_history_pos >= (int)g_history.size()) {
                    g_history_pos = -1;
                }
            }
        }
        if (prev != g_history_pos) {
            const char *s = (g_history_pos >= 0) ? g_history[g_history_pos].c_str() : "";
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, s);
        }
    }
    return 0;
}

}  // namespace

extern "C" void imgui_console_set_submit_callback(imgui_console_submit_fn cb) {
    g_submit = cb;
}

extern "C" void imgui_console_add_log(const char *text, bool is_error) {
    if (!text) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pending.push_back(ConsoleItem{std::string(text), is_error});
}

extern "C" void imgui_console_clear(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pending.clear();
    g_want_clear = true;
}

extern "C" void imgui_console_focus_input(void) {
    g_focus_input = true;
}

extern "C" void imgui_console_draw(bool *p_open) {
    // Drain the cross-thread inbox into the display list.
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_want_clear) {
            g_items.clear();
            g_want_clear = false;
        }
        if (!g_pending.empty()) {
            for (auto &it : g_pending) g_items.push_back(std::move(it));
            g_pending.clear();
            g_scroll_to_bottom = true;
        }
    }
    if (g_items.size() > kMaxItems) {
        g_items.erase(g_items.begin(), g_items.begin() + (g_items.size() - kMaxItems));
    }

    // Apply a queued clipboard paste. This runs the frame AFTER the Paste button
    // was clicked, when the input field is no longer active (the button click
    // deactivated it) — so appending to the buffer won't be clobbered by the
    // field's edit state. Refocusing then makes the field reload the new buffer.
    if (g_has_pending_paste) {
        size_t cur = strlen(g_input);
        if (cur + 1 < sizeof(g_input)) {
            strncat(g_input, g_pending_paste.c_str(), sizeof(g_input) - cur - 1);
        }
        g_pending_paste.clear();
        g_has_pending_paste = false;
        g_focus_input = true;
    }

    ImGui::SetNextWindowSize(ImVec2(680, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("BG3SE Console", p_open)) {
        ImGui::End();
        return;
    }

    if (ImGui::SmallButton("Clear")) imgui_console_clear();
    ImGui::SameLine();
    const bool copy = ImGui::SmallButton("Copy");
    ImGui::SameLine();
    if (ImGui::SmallButton("Paste")) {
        const char *clip = ImGui::GetClipboardText();
        if (clip && clip[0]) {
            g_pending_paste = clip;     // applied next frame (see top of draw)
            g_has_pending_paste = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Level Up")) {
        if (g_submit) g_submit(kLevelUpLua);
    }
    ImGui::Separator();

    // Output region (leave room for one input line below).
    const float footer_h = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("scroll", ImVec2(0, -footer_h), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (copy) ImGui::LogToClipboard();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
    for (const auto &it : g_items) {
        if (it.is_error) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextUnformatted(it.text.c_str());
        if (it.is_error) ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar();
    if (copy) ImGui::LogFinish();
    // Auto-scroll when new content arrived or we're already pinned to the bottom.
    if (g_scroll_to_bottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    g_scroll_to_bottom = false;
    ImGui::EndChild();
    ImGui::Separator();

    // Input line.
    bool reclaim_focus = false;

    // Focus the input BEFORE submitting it, so on reactivation it reloads the
    // buffer (including any just-applied paste). Used on open and after a paste.
    if (g_focus_input) {
        ImGui::SetKeyboardFocusHere();  // focuses the next widget (the InputText)
        g_focus_input = false;
    }

    ImGui::PushItemWidth(-1.0f);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputText("##cmd", g_input, sizeof(g_input), flags, input_text_callback)) {
        if (g_input[0] != '\0') {
            std::string echo = std::string("> ") + g_input;
            imgui_console_add_log(echo.c_str(), false);
            g_history.push_back(std::string(g_input));
            g_history_pos = -1;
            if (g_submit) g_submit(g_input);
        }
        g_input[0] = '\0';
        reclaim_focus = true;
    }
    ImGui::PopItemWidth();
    ImGui::SetItemDefaultFocus();
    if (reclaim_focus) {
        ImGui::SetKeyboardFocusHere(-1);  // refocus the input after submitting
    }

    ImGui::End();
}
