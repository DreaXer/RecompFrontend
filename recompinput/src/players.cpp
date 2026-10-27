#include "recompinput/recompinput.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"
#include "recompinput/input_events.h"
#include "./json.h"
#include <vector>
#include <cstdio>
#include <filesystem>
#include "ultramodern/ultramodern.hpp"
#include "composites/ui_assign_players_modal.h"
#include "config/ui_config_page_controls.h"

namespace recompinput {

void Player::button_pressed() {
    last_button_press_timestamp = ultramodern::time_since_start();
}

using players_array = std::array<Player, max_num_players_supported>;
class PlayerArray : public players_array {
private:
    size_t count = 0;
public:
    PlayerArray() : players_array{}, count(0) {}
    PlayerArray(const PlayerArray& other) : players_array(other), count(other.count) {}

    size_t get_count() const {
        return count;
    }

    void add_keyboard_player() {
        if (count < max_num_players_supported) {
            Player &player = (*this)[count++];
            player = Player();
            player.keyboard_enabled = true;
            player.button_pressed();
        }
    }

    void add_controller_player(SDL_GameController* controller) {
        if (count < max_num_players_supported) {
            Player &player = (*this)[count++];
            player = Player();
            player.controller = controller;
            player.button_pressed();
        }
    }

    bool player_is_assigned(int player_index) const {
        return player_index >= 0 && player_index < max_num_players_supported && player_index < count;
    }
};

static struct {
    bool single_player_mode = false;
    bool is_assigning = false;
    size_t min_players = 1;
    size_t max_players = 4;
    PlayerArray players;
    PlayerArray temp_players;

    PlayerArray& get_player_array(bool temp) {
        return temp ? temp_players : players;
    }

    bool queue_close_player_assignment_modal = false;
} PlayerState;

// ---- Persistent player assignment ----
namespace {
    struct SavedPlayer {
        bool keyboard = false;
        uint64_t hash = 0;
        std::string serial;
        int occurrence = 0; // n-th connected controller (SDL device order) with the same hash
    };
    std::vector<SavedPlayer> saved_players;
    std::filesystem::path assignment_file_path;

    // Connected (opened) game controllers in SDL device order.
    std::vector<SDL_GameController*> connected_controllers() {
        std::vector<SDL_GameController*> result;
        int count = SDL_NumJoysticks();
        for (int i = 0; i < count; i++) {
            if (SDL_IsGameController(i)) {
                SDL_GameController* gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
                if (gc != nullptr) {
                    result.push_back(gc);
                }
            }
        }
        return result;
    }

    void save_assignment_file(const PlayerArray& array) {
        if (assignment_file_path.empty()) {
            return;
        }
        nlohmann::json out = nlohmann::json::array();
        auto connected = connected_controllers();
        saved_players.clear();
        for (size_t i = 0; i < array.get_count(); i++) {
            const Player& player = array[i];
            SavedPlayer saved;
            nlohmann::json entry;
            if (player.controller != nullptr) {
                ControllerGUID guid = profiles::get_guid_from_sdl_controller(player.controller);
                saved.hash = guid.hash;
                saved.serial = guid.serial;
                for (SDL_GameController* gc : connected) {
                    if (gc == player.controller) {
                        break;
                    }
                    if (profiles::get_guid_from_sdl_controller(gc).hash == guid.hash) {
                        saved.occurrence++;
                    }
                }
                entry["type"] = "controller";
                entry["hash"] = std::to_string(saved.hash);
                entry["serial"] = saved.serial;
                entry["occurrence"] = saved.occurrence;
            } else {
                saved.keyboard = true;
                entry["type"] = "keyboard";
            }
            saved_players.push_back(saved);
            out.push_back(entry);
        }
        nlohmann::json root;
        root["version"] = 1;
        root["players"] = out;
        bool ok = recompinput::save_json_with_backups(assignment_file_path, root);
        printf("[assignment] saved %zu player(s) to %s: %s\n", array.get_count(), assignment_file_path.string().c_str(), ok ? "ok" : "FAILED");
        fflush(stdout);
    }

    void load_assignment_file() {
        saved_players.clear();
        nlohmann::json root;
        if (assignment_file_path.empty() || !recompinput::read_json_with_backups(assignment_file_path, root)) {
            return;
        }
        auto it = root.find("players");
        if (it == root.end() || !it->is_array()) {
            return;
        }
        for (const nlohmann::json& entry : *it) {
            SavedPlayer saved;
            std::string type = entry.value("type", std::string());
            if (type == "keyboard") {
                saved.keyboard = true;
            } else if (type == "controller") {
                try {
                    saved.hash = std::stoull(entry.value("hash", std::string("0")));
                } catch (...) {
                    saved_players.clear();
                    return;
                }
                saved.serial = entry.value("serial", std::string());
                saved.occurrence = entry.value("occurrence", 0);
            } else {
                saved_players.clear();
                return;
            }
            saved_players.push_back(saved);
        }
    }
}

size_t players::get_number_of_assigned_players() {
    return PlayerState.players.get_count();
}

size_t players::get_max_number_of_players() {
    if (PlayerState.single_player_mode) {
        return 1;
    }
    return PlayerState.max_players;
}

bool players::is_single_player_mode() {
    return PlayerState.single_player_mode;
}

void players::set_single_player_mode(bool single_player) {
    if (!single_player && single_player != PlayerState.single_player_mode) {
        purge_deferred_controller_profiles();
    }

    PlayerState.single_player_mode = single_player;
}

void players::set_min_number_of_players(size_t min_players) {
    assert(min_players >= 1 && min_players <= PlayerState.max_players);
    PlayerState.min_players = min_players;
}

void players::set_max_number_of_players(size_t max_players) {
    assert(max_players >= PlayerState.min_players && max_players <= recompinput::max_num_players_supported);
    PlayerState.max_players = max_players;
}

void players::set_player_count_range(size_t min_players, size_t max_players) {
    assert(min_players >= 1 && max_players <= recompinput::max_num_players_supported && min_players <= max_players);
    PlayerState.min_players = min_players;
    PlayerState.max_players = max_players;
}

const recompinput::Player& players::get_player(int player_index, bool temp_player) {
    return PlayerState.get_player_array(temp_player)[player_index];
}

bool players::get_player_is_assigned(int player_index, bool temp_player) {
    return PlayerState.get_player_array(temp_player).player_is_assigned(player_index);
}

bool players::has_enough_players_assigned() {
    return PlayerState.players.get_count() >= PlayerState.min_players;
}

InputDevice players::get_player_input_device(int player_index, bool temp_player) {
    auto player_array = PlayerState.get_player_array(temp_player);
    if (!player_array.player_is_assigned(player_index)) {
        return InputDevice::COUNT;
    }

    const auto& assigned_player = player_array[player_index];
    if (assigned_player.controller != nullptr) {
        return InputDevice::Controller;
    } else if (assigned_player.keyboard_enabled) {
        return InputDevice::Keyboard;
    } else {
        return InputDevice::COUNT;
    }
}


static void apply_profiles_to_players();

void players::set_assignment_file_path(const std::filesystem::path& path) {
    assignment_file_path = path;
    load_assignment_file();
    printf("[assignment] file %s, %zu saved player(s) loaded\n", assignment_file_path.string().c_str(), saved_players.size());
    fflush(stdout);
}

void players::restore_saved_assignment() {
    // Only restore when nothing is assigned yet, or when an assigned controller was unplugged / went to sleep (the assignment
    // keeps its dead SDL pointer, the controller comes back as a new device and was never re-assigned: players had to be
    // re-assigned by hand). Wait until every saved controller is connected.
    bool stale = false;
    for (size_t i = 0; i < PlayerState.players.get_count(); i++) {
        SDL_GameController* gc = PlayerState.players[i].controller;
        if (gc != nullptr && SDL_GameControllerGetAttached(gc) == SDL_FALSE) {
            stale = true;
        }
    }
    if ((PlayerState.players.get_count() > 0 && !stale) || saved_players.empty() || playerassignment::is_active()) {
        return;
    }
    if (stale) {
        printf("[assignment] an assigned controller was disconnected: trying to restore the saved assignment\n");
        fflush(stdout);
    }

    auto connected = connected_controllers();
    std::vector<bool> used(connected.size(), false);
    std::vector<int> match(saved_players.size(), -1);
    for (size_t p = 0; p < saved_players.size(); p++) {
        const SavedPlayer& saved = saved_players[p];
        if (saved.keyboard) {
            continue;
        }
        int found = -1;
        // 1) same hash and same non-empty serial
        if (!saved.serial.empty()) {
            for (size_t i = 0; i < connected.size(); i++) {
                ControllerGUID guid = profiles::get_guid_from_sdl_controller(connected[i]);
                if (!used[i] && guid.hash == saved.hash && guid.serial == saved.serial) {
                    found = (int)i;
                    break;
                }
            }
        }
        // 2) n-th controller with the same hash
        if (found < 0) {
            int seen = 0;
            for (size_t i = 0; i < connected.size(); i++) {
                if (profiles::get_guid_from_sdl_controller(connected[i]).hash == saved.hash) {
                    if (seen == saved.occurrence && !used[i]) {
                        found = (int)i;
                        break;
                    }
                    seen++;
                }
            }
        }
        if (found >= 0) {
            used[found] = true;
            match[p] = found;
        }
    }
    // 3) a controller that comes back can report another GUID (crc/version/serial change after a reconnection, e.g. Bluetooth):
    // give the remaining connected controllers to the remaining saved players, in order.
    PlayerArray restored;
    {
        size_t next = 0;
        for (size_t p = 0; p < saved_players.size(); p++) {
            if (saved_players[p].keyboard || match[p] >= 0) {
                continue;
            }
            while (next < connected.size() && used[next]) {
                next++;
            }
            if (next >= connected.size()) {
                printf("[assignment] restore waiting: a saved controller isn't connected yet (%zu connected)\n", connected.size());
                fflush(stdout);
                return;
            }
            printf("[assignment] saved controller %zu not matched by GUID: using connected controller %zu\n", p, next);
            used[next] = true;
            match[p] = (int)next;
        }
    }
    for (size_t p = 0; p < saved_players.size(); p++) {
        if (saved_players[p].keyboard) {
            restored.add_keyboard_player();
        } else {
            restored.add_controller_player(connected[match[p]]);
        }
    }

    // The controller profiles must exist for every controller before they can be bound to players. A controller that
    // was detected before the controls config was loaded may have no profile yet: create it like the "device added" event does.
    for (size_t i = 0; i < restored.get_count(); i++) {
        if (restored[i].controller == nullptr) {
            continue;
        }
        ControllerGUID guid = profiles::get_guid_from_sdl_controller(restored[i].controller);
        if (profiles::get_controller_by_guid(guid) < 0) {
            std::string key = profiles::get_string_from_controller_guid(guid);
            int profile_index = profiles::get_input_profile_by_key(key);
            if (profile_index < 0) {
                profile_index = profiles::add_input_profile(key, "Controller", InputDevice::Controller, false);
                profiles::reset_profile_bindings(profile_index, InputDevice::Controller);
            }
            profiles::add_controller(guid, profile_index);
        }
        if (profiles::get_controller_profile_index_from_sdl_controller(restored[i].controller) < 0) {
            printf("[assignment] restore failed: no input profile for controller %zu\n", i);
            fflush(stdout);
            return;
        }
    }

    PlayerState.players = restored;
    apply_profiles_to_players();
    printf("[assignment] restored %zu player(s)\n", restored.get_count());
    fflush(stdout);
    if (recompui::controls_page != nullptr) {
        recompui::controls_page->force_update();
    }
}

// playerassignment start

bool playerassignment::is_active() {
    return PlayerState.is_assigning;
}

void playerassignment::start(void) {
    PlayerState.is_assigning = true;
    PlayerState.temp_players = PlayerArray{};
}

void playerassignment::stop(void) {
    PlayerState.is_assigning = false;
}

void playerassignment::stop_and_close_modal() {
    playerassignment::stop();
    PlayerState.queue_close_player_assignment_modal = true;
}

static void apply_profiles_to_players() {
    for (int i = 0; i < players::get_number_of_assigned_players(); i++) {
        Player &player = PlayerState.players[i];
        if (player.controller != nullptr) {
            int cont_profile_index = profiles::get_controller_profile_index_from_sdl_controller(player.controller);
            if (cont_profile_index >= 0) {
                profiles::set_input_profile_for_player(i, cont_profile_index, InputDevice::Controller);
            }
        } else {
            profiles::set_input_profile_for_player(i, profiles::get_or_create_mp_keyboard_profile_index(i), InputDevice::Keyboard);
        }
    }
}

void playerassignment::commit_player_assignment() {
    stop_and_close_modal();

    PlayerState.players = PlayerState.temp_players;
    apply_profiles_to_players();
    save_assignment_file(PlayerState.players);
}

void playerassignment::add_keyboard_player() {
    PlayerState.temp_players.add_keyboard_player();
}

bool playerassignment::met_assignment_requirements() {
    return PlayerState.temp_players.get_count() >= PlayerState.min_players;
}

bool playerassignment::is_blocking_input() {
    return playerassignment::is_active() && !playerassignment::met_assignment_requirements();
}

bool playerassignment::is_player_currently_assigning(int player_index) {
    return playerassignment::is_active() && player_index == (int)(PlayerState.temp_players.get_count());
}

bool playerassignment::was_keyboard_assigned() {
    size_t num_temp = PlayerState.temp_players.size();
    if (num_temp == 0) {
        return false;
    }

    for (size_t i = 0; i < num_temp; i++) {
        if (PlayerState.temp_players[i].keyboard_enabled) {
            return true;
        }
    }

    return false;
}

std::chrono::steady_clock::duration playerassignment::get_player_time_since_last_button_press(int player_index) {
    if (!PlayerState.temp_players.player_is_assigned(player_index)) {
        return std::chrono::steady_clock::duration::zero();
    }
    return ultramodern::time_since_start() - PlayerState.temp_players[player_index].last_button_press_timestamp;
}

void playerassignment::process_sdl_event(SDL_Event* event) {
    if (PlayerState.queue_close_player_assignment_modal) {
        recompui::AssignPlayersModal::close();
        PlayerState.queue_close_player_assignment_modal = false;
        if (recompui::controls_page != nullptr) {
            recompui::controls_page->force_update();
        }
        return;
    }

    if (!playerassignment::is_active()) {
        return;
    }

    switch (event->type) {
    case SDL_EventType::SDL_KEYDOWN: {
        SDL_KeyboardEvent* keyevent = &event->key;

        switch (keyevent->keysym.scancode) {
        case SDL_Scancode::SDL_SCANCODE_ESCAPE:
            // TODO: Restore previous assignment?
            playerassignment::stop();
            return;
        case SDL_Scancode::SDL_SCANCODE_RIGHT:
        case SDL_Scancode::SDL_SCANCODE_LEFT:
        case SDL_Scancode::SDL_SCANCODE_UP:
        case SDL_Scancode::SDL_SCANCODE_DOWN:
            break;
        default:
            if (playerassignment::was_keyboard_assigned()) {
                for (size_t i = 0; i < PlayerState.temp_players.size(); i++) {
                    if (PlayerState.temp_players[i].keyboard_enabled && PlayerState.temp_players[i].controller == nullptr) {
                        PlayerState.temp_players[i].button_pressed();
                    }
                }
            } else {
                playerassignment::add_keyboard_player();
            }
            break;
        }
        break;
    }
    case SDL_EventType::SDL_CONTROLLERBUTTONDOWN: {
        SDL_ControllerButtonEvent* button_event = &event->cbutton;
        SDL_JoystickID joystick_id = button_event->which;
        auto controller = recompinput::get_controller_from_joystick_id(joystick_id);

        bool can_be_mapped = true;
        for (size_t i = 0; i < PlayerState.temp_players.size(); i++) {
            if (PlayerState.temp_players[i].controller == controller) {
                can_be_mapped = false;
                PlayerState.temp_players[i].button_pressed();
                break;
            }
        }

        if (can_be_mapped) {
            PlayerState.temp_players.add_controller_player(controller);
        }

        break;
    }
    }
}
// player assignment end

}
