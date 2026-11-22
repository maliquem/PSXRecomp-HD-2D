#ifndef PSX_CONTROLLER_TYPE_H
#define PSX_CONTROLLER_TYPE_H

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

/* SDL2/3 does not expose a consistently useful wheel class for every mapping.
 * This is a conservative name hint; only mapped controllers with known wheel
 * family names are promoted to the JogCon device protocol. */
inline bool psx_controller_name_is_wheel(const char* name) {
    if (!name || !*name) return false;
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static const char* tokens[] = {
        "wheel", "g29", "g920", "g923", "g27", "g25", "driving force",
        "t150", "t300", "t500", "t248", "tx racing", "thrustmaster",
        "fanatec", "moza"
    };
    for (const char* token : tokens)
        if (normalized.find(token) != std::string::npos) return true;
    return false;
}

/* Pure reopen policy so unplug and launcher-selected device changes can be
 * regression-tested without a physical SDL controller. */
inline bool psx_controller_handle_needs_close(bool controller_selected,
                                               bool handle_open,
                                               bool handle_attached,
                                               const char* selected_guid,
                                               const char* active_guid) {
    if (!controller_selected) return true;
    if (!handle_open) return false;
    if (!handle_attached) return true;
    return selected_guid && selected_guid[0] && active_guid && active_guid[0] &&
           std::strcmp(selected_guid, active_guid) != 0;
}

/* A physical Valve controller (Steam Controller 2015 / 2026, Steam Deck) seen
 * directly through SDL's HIDAPI driver, i.e. NOT Steam Input's virtual pad
 * (28DE:11FF). Outside a Steam launch the Steam client still applies its
 * Desktop Configuration to such a pad and injects keyboard/mouse input
 * (B = Escape, Back = Tab, A = Enter, triggers = mouse clicks) next to the
 * gamepad reports SDL reads. */
inline bool psx_controller_is_physical_valve_pad(unsigned vendor,
                                                 unsigned product) {
    return vendor == 0x28DEu && product != 0x11FFu;
}

/* Steam desktop-layout echo guard: while P1's pad is a physical Valve pad
 * and it has input held (or released within the trailing window), keyboard
 * and mouse input is Steam's synthesized echo of that same press and must
 * not reach the game, the host hotkeys (Tab turbo) or the menu (Escape). */
inline bool psx_steam_desktop_echo_active(bool p1_physical_valve_pad,
                                          bool pad_input_held,
                                          unsigned long long now_ms,
                                          unsigned long long last_held_ms,
                                          unsigned long long window_ms) {
    if (!p1_physical_valve_pad) return false;
    if (pad_input_held) return true;
    return last_held_ms != 0 && now_ms >= last_held_ms &&
           now_ms - last_held_ms <= window_ms;
}

#endif
