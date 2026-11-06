#include "gfx/backends/sdlgpu/sdlgpu_input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {
namespace {
enum : uint32_t {
    Select = 0x0001u, L3 = 0x0002u, R3 = 0x0004u, Start = 0x0008u,
    Up = 0x0010u, Right = 0x0020u, Down = 0x0040u, Left = 0x0080u,
    L2 = 0x0100u, R2 = 0x0200u, L1 = 0x0400u, R1 = 0x0800u,
    Triangle = 0x1000u, Circle = 0x2000u, Cross = 0x4000u, Square = 0x8000u
};

struct PadInputInfo {
    uint32_t mask;
    const char *name;
    const char *key;
};
constexpr PadInputInfo kInputs[kPadInputCount] = {
    {Cross, "Cross", "cross"},
    {Circle, "Circle", "circle"},
    {Square, "Square", "square"},
    {Triangle, "Triangle", "triangle"},
    {Up, "D-pad up", "up"},
    {Down, "D-pad down", "down"},
    {Left, "D-pad left", "left"},
    {Right, "D-pad right", "right"},
    {L1, "L1", "l1"},
    {R1, "R1", "r1"},
    {L2, "L2", "l2"},
    {R2, "R2", "r2"},
    {L3, "L3", "l3"},
    {R3, "R3", "r3"},
    {Start, "Start", "start"},
    {Select, "Select", "select"},
    {0u, "Left stick up", "lstick_up"},
    {0u, "Left stick down", "lstick_down"},
    {0u, "Left stick left", "lstick_left"},
    {0u, "Left stick right", "lstick_right"},
    {0u, "Right stick up", "rstick_up"},
    {0u, "Right stick down", "rstick_down"},
    {0u, "Right stick left", "rstick_left"},
    {0u, "Right stick right", "rstick_right"},
    {0u, "Walk (hold)", "walk"},
};

constexpr int kAxisFull = 32768;

size_t index(PadInput input) { return static_cast<size_t>(input); }

// How hard a binding pushes its input, 0 to 32768. Keys and buttons are all
// or nothing; an axis pushes only in its own direction; the wheel is only
// ever pressed (handleEvent()).
int bindingValue(const PadBinding &binding, SDL_Gamepad *gamepad, const bool *keys,
                 SDL_MouseButtonFlags mouse = 0u) {
    switch (binding.kind) {
    case PadBinding::Kind::Key:
        return keys && binding.code >= 0 && binding.code < SDL_SCANCODE_COUNT && keys[binding.code]
                   ? kAxisFull : 0;
    case PadBinding::Kind::Mouse:
        return binding.code >= SDL_BUTTON_LEFT && binding.code <= SDL_BUTTON_X2 &&
                       (mouse & SDL_BUTTON_MASK(binding.code)) != 0u
                   ? kAxisFull : 0;
    case PadBinding::Kind::Wheel:
        return 0;
    case PadBinding::Kind::Button:
        return gamepad && SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(binding.code))
                   ? kAxisFull : 0;
    case PadBinding::Kind::Axis: {
        if (!gamepad)
            return 0;
        const int value = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(binding.code)) *
                          binding.direction;
        return std::max(value, 0);
    }
    case PadBinding::Kind::None:
        break;
    }
    return 0;
}

// Opposing directions cancel, then the drift dead zone snaps small values to
// centre; past it the controller's full travel is kept.
uint8_t stickByte(int raw, int deadZone) {
    raw = std::clamp(raw, -kAxisFull, kAxisFull - 1);
    return std::abs(raw) < deadZone ? 128u : static_cast<uint8_t>((raw + kAxisFull) >> 8);
}
} // namespace

PadBinding PadBinding::key(SDL_Scancode scancode) {
    return {Kind::Key, static_cast<int16_t>(scancode), 1};
}
PadBinding PadBinding::button(SDL_GamepadButton button) {
    return {Kind::Button, static_cast<int16_t>(button), 1};
}
PadBinding PadBinding::axis(SDL_GamepadAxis axis, int direction) {
    return {Kind::Axis, static_cast<int16_t>(axis), static_cast<int8_t>(direction < 0 ? -1 : 1)};
}
PadBinding PadBinding::mouse(int button) {
    return {Kind::Mouse, static_cast<int16_t>(button), 1};
}
PadBinding PadBinding::wheel(int direction) {
    return {Kind::Wheel, 0, static_cast<int8_t>(direction < 0 ? -1 : 1)};
}

PadConfig PadConfig::defaults() {
    PadConfig config;
    const auto pad = [&](PadInput input, PadBinding binding) { config.controller[index(input)][0] = binding; };
    const auto keys = [&](PadInput input, SDL_Scancode first, SDL_Scancode second = SDL_SCANCODE_UNKNOWN) {
        config.keyboard[index(input)][0] = PadBinding::key(first);
        if (second != SDL_SCANCODE_UNKNOWN)
            config.keyboard[index(input)][1] = PadBinding::key(second);
    };
    pad(PadInput::Cross, PadBinding::button(SDL_GAMEPAD_BUTTON_SOUTH));
    pad(PadInput::Circle, PadBinding::button(SDL_GAMEPAD_BUTTON_EAST));
    pad(PadInput::Square, PadBinding::button(SDL_GAMEPAD_BUTTON_WEST));
    pad(PadInput::Triangle, PadBinding::button(SDL_GAMEPAD_BUTTON_NORTH));
    pad(PadInput::Up, PadBinding::button(SDL_GAMEPAD_BUTTON_DPAD_UP));
    pad(PadInput::Down, PadBinding::button(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    pad(PadInput::Left, PadBinding::button(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
    pad(PadInput::Right, PadBinding::button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
    pad(PadInput::L1, PadBinding::button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    pad(PadInput::R1, PadBinding::button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    pad(PadInput::L2, PadBinding::axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1));
    pad(PadInput::R2, PadBinding::axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1));
    pad(PadInput::L3, PadBinding::button(SDL_GAMEPAD_BUTTON_LEFT_STICK));
    pad(PadInput::R3, PadBinding::button(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
    pad(PadInput::Start, PadBinding::button(SDL_GAMEPAD_BUTTON_START));
    pad(PadInput::Select, PadBinding::button(SDL_GAMEPAD_BUTTON_BACK));
    pad(PadInput::LeftStickUp, PadBinding::axis(SDL_GAMEPAD_AXIS_LEFTY, -1));
    pad(PadInput::LeftStickDown, PadBinding::axis(SDL_GAMEPAD_AXIS_LEFTY, 1));
    pad(PadInput::LeftStickLeft, PadBinding::axis(SDL_GAMEPAD_AXIS_LEFTX, -1));
    pad(PadInput::LeftStickRight, PadBinding::axis(SDL_GAMEPAD_AXIS_LEFTX, 1));
    pad(PadInput::RightStickUp, PadBinding::axis(SDL_GAMEPAD_AXIS_RIGHTY, -1));
    pad(PadInput::RightStickDown, PadBinding::axis(SDL_GAMEPAD_AXIS_RIGHTY, 1));
    pad(PadInput::RightStickLeft, PadBinding::axis(SDL_GAMEPAD_AXIS_RIGHTX, -1));
    pad(PadInput::RightStickRight, PadBinding::axis(SDL_GAMEPAD_AXIS_RIGHTX, 1));

    keys(PadInput::Up, SDL_SCANCODE_UP);
    keys(PadInput::Down, SDL_SCANCODE_DOWN);
    keys(PadInput::Left, SDL_SCANCODE_LEFT);
    keys(PadInput::Right, SDL_SCANCODE_RIGHT);
    keys(PadInput::Cross, SDL_SCANCODE_X, SDL_SCANCODE_SPACE);
    keys(PadInput::Circle, SDL_SCANCODE_C, SDL_SCANCODE_ESCAPE);
    keys(PadInput::Square, SDL_SCANCODE_Z, SDL_SCANCODE_KP_0);
    keys(PadInput::Triangle, SDL_SCANCODE_V, SDL_SCANCODE_KP_1);
    keys(PadInput::L1, SDL_SCANCODE_Q);
    keys(PadInput::R1, SDL_SCANCODE_E);
    keys(PadInput::L2, SDL_SCANCODE_LSHIFT);
    keys(PadInput::R2, SDL_SCANCODE_RSHIFT);
    keys(PadInput::L3, SDL_SCANCODE_R);
    keys(PadInput::R3, SDL_SCANCODE_F);
    keys(PadInput::Start, SDL_SCANCODE_RETURN);
    keys(PadInput::Select, SDL_SCANCODE_TAB);
    keys(PadInput::LeftStickUp, SDL_SCANCODE_W);
    keys(PadInput::LeftStickDown, SDL_SCANCODE_S);
    keys(PadInput::LeftStickLeft, SDL_SCANCODE_A);
    keys(PadInput::LeftStickRight, SDL_SCANCODE_D);
    keys(PadInput::RightStickUp, SDL_SCANCODE_I);
    keys(PadInput::RightStickDown, SDL_SCANCODE_K);
    keys(PadInput::RightStickLeft, SDL_SCANCODE_J);
    keys(PadInput::RightStickRight, SDL_SCANCODE_L);
    keys(PadInput::Walk, SDL_SCANCODE_LCTRL);
    return config;
}

uint32_t padButtonMask(PadInput input) {
    return input < PadInput::Count ? kInputs[index(input)].mask : 0u;
}
const char *padInputName(PadInput input) {
    return input < PadInput::Count ? kInputs[index(input)].name : "";
}
const char *padInputKey(PadInput input) {
    return input < PadInput::Count ? kInputs[index(input)].key : "";
}

namespace {
// The mouse's buttons in the settings file, by SDL's numbers (1 to 5).
constexpr const char *kMouseNames[] = {"left", "middle", "right", "x1", "x2"};
} // namespace

std::string padBindingToString(const PadBinding &binding) {
    switch (binding.kind) {
    case PadBinding::Kind::Mouse:
        if (binding.code >= SDL_BUTTON_LEFT && binding.code <= SDL_BUTTON_X2)
            return std::string("mouse:") + kMouseNames[binding.code - SDL_BUTTON_LEFT];
        break;
    case PadBinding::Kind::Wheel:
        return binding.direction < 0 ? "wheel:down" : "wheel:up";
    case PadBinding::Kind::Key:
        if (const char *name = SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.code)); name && *name)
            return std::string("key:") + name;
        break;
    case PadBinding::Kind::Button:
        if (const char *name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(binding.code)))
            return std::string("button:") + name;
        break;
    case PadBinding::Kind::Axis:
        if (const char *name = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(binding.code)))
            return std::string("axis:") + (binding.direction < 0 ? "-" : "+") + name;
        break;
    case PadBinding::Kind::None:
        break;
    }
    return "none";
}

std::optional<PadBinding> padBindingFromString(const std::string &text) {
    if (text == "none")
        return PadBinding{};
    const auto colon = text.find(':');
    if (colon == std::string::npos)
        return std::nullopt;
    const std::string kind = text.substr(0, colon);
    std::string name = text.substr(colon + 1);
    if (kind == "key") {
        if (const SDL_Scancode scancode = SDL_GetScancodeFromName(name.c_str()); scancode != SDL_SCANCODE_UNKNOWN)
            return PadBinding::key(scancode);
    } else if (kind == "button") {
        if (const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
            button != SDL_GAMEPAD_BUTTON_INVALID)
            return PadBinding::button(button);
    } else if (kind == "axis" && name.size() > 1u && (name[0] == '+' || name[0] == '-')) {
        const int direction = name[0] == '-' ? -1 : 1;
        if (const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name.c_str() + 1);
            axis != SDL_GAMEPAD_AXIS_INVALID)
            return PadBinding::axis(axis, direction);
    } else if (kind == "mouse") {
        for (int button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; ++button)
            if (name == kMouseNames[button - SDL_BUTTON_LEFT])
                return PadBinding::mouse(button);
    } else if (kind == "wheel" && (name == "up" || name == "down")) {
        return PadBinding::wheel(name == "down" ? -1 : 1);
    }
    return std::nullopt;
}

SdlPadInput::~SdlPadInput() {
    for (auto &gamepad : m_gamepads)
        SDL_CloseGamepad(gamepad.handle);
    if (m_initialized)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

bool SdlPadInput::initialize(std::string &error) {
    if (m_initialized)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        error = std::string("SDL_InitSubSystem(gamepad): ") + SDL_GetError();
        return false;
    }
    m_initialized = true;
    openAll();
    return true;
}

void SdlPadInput::openGamepad(SDL_JoystickID id) {
    if (findGamepad(id))
        return;
    SDL_Gamepad *handle = SDL_OpenGamepad(id);
    if (!handle)
        return;
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    const char *serial = SDL_GetGamepadSerial(handle);
    m_gamepads.push_back({handle, guid, serial ? serial : "", {}});
    std::fprintf(stderr, "[pad] connected: %s\n", SDL_GetGamepadName(handle));
}

void SdlPadInput::openAll() {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i)
        openGamepad(ids[i]);
    SDL_free(ids);
}

SdlPadInput::Gamepad *SdlPadInput::findGamepad(SDL_JoystickID id) {
    for (auto &gamepad : m_gamepads)
        if (SDL_GetGamepadID(gamepad.handle) == id)
            return &gamepad;
    return nullptr;
}

bool SdlPadInput::gamepadActive(const Gamepad &gamepad) const {
    switch (m_config.controllers) {
    case ControllerSelection::Any:
        return true;
    case ControllerSelection::One:
        return chosenGamepad() == &gamepad;
    case ControllerSelection::None:
        break;
    }
    return false;
}

const SdlPadInput::Gamepad *SdlPadInput::chosenGamepad() const {
    // The device picked this session; else the saved serial, which must
    // match when there is one; else the first connected of the model.
    const Gamepad *found = nullptr;
    for (const auto &candidate : m_gamepads) {
        if (candidate.guid != m_config.controllerGuid)
            continue;
        if (m_config.controllerInstance != 0 && SDL_GetGamepadID(candidate.handle) == m_config.controllerInstance)
            return &candidate;
        if (!found && (m_config.controllerSerial.empty() || candidate.serial == m_config.controllerSerial))
            found = &candidate;
    }
    return found;
}

void SdlPadInput::clearState() {
    m_held = m_pressed = 0u;
    m_sticks = 0x80808080u;
    m_walking = false;
}

void SdlPadInput::setConfig(const PadConfig &config) {
    m_config = config;
    for (auto &gamepad : m_gamepads)
        gamepad.axisPressed.fill(false);
}

void SdlPadInput::suppressHeld() {
    // The pad reads idle while the menu is open, so what is held is only
    // known at the next focused poll.
    m_suppressPending = true;
    m_pressed = 0u;
}

std::vector<ControllerInfo> SdlPadInput::controllers() const {
    std::vector<ControllerInfo> result;
    for (const auto &gamepad : m_gamepads) {
        ControllerInfo info;
        info.id = SDL_GetGamepadID(gamepad.handle);
        const char *name = SDL_GetGamepadName(gamepad.handle);
        info.name = name ? name : "Controller";
        info.guid = gamepad.guid;
        info.serial = gamepad.serial;
        info.type = SDL_GetGamepadType(gamepad.handle);
        info.active = gamepadActive(gamepad);
        info.power = SDL_GetGamepadPowerInfo(gamepad.handle, &info.batteryPercent);
        result.push_back(std::move(info));
    }
    return result;
}

void SdlPadInput::handleEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && m_config.keyboardEnabled) {
        for (size_t input = 0; input < kPadInputCount; ++input)
            for (const auto &binding : m_config.keyboard[input])
                if (binding.kind == PadBinding::Kind::Key && binding.code == event.key.scancode) {
                    m_pressed |= kInputs[input].mask;
                    m_keyboardMouseAt = SDL_GetTicks();
                }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && m_config.keyboardEnabled) {
        m_keyboardMouseAt = SDL_GetTicks();
        for (size_t input = 0; input < kPadInputCount; ++input)
            for (const auto &binding : m_config.keyboard[input])
                if (binding.kind == PadBinding::Kind::Mouse && binding.code == event.button.button)
                    m_pressed |= kInputs[input].mask;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL && m_config.keyboardEnabled) {
        float turn = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            turn = -turn;
        if (turn != 0.0f) {
            m_keyboardMouseAt = SDL_GetTicks();
            for (size_t input = 0; input < kPadInputCount; ++input)
                for (const auto &binding : m_config.keyboard[input])
                    if (binding.kind == PadBinding::Kind::Wheel && binding.direction == (turn > 0.0f ? 1 : -1))
                        m_pressed |= kInputs[input].mask;
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        m_mouseMotionAt = SDL_GetTicks();
        return;
    }
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        openGamepad(event.gdevice.which);
        return;
    }
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        const auto it = std::find_if(m_gamepads.begin(), m_gamepads.end(), [&](const Gamepad &gamepad) {
            return SDL_GetGamepadID(gamepad.handle) == event.gdevice.which;
        });
        if (it != m_gamepads.end()) {
            const bool wasActive = gamepadActive(*it);
            SDL_CloseGamepad(it->handle);
            m_gamepads.erase(it);
            if (wasActive)
                clearState();
        }
        return;
    }
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const Gamepad *gamepad = findGamepad(event.gbutton.which);
        if (!gamepad || !gamepadActive(*gamepad))
            return;
        m_controllerAt = SDL_GetTicks();
        for (size_t input = 0; input < kPadInputCount; ++input)
            for (const auto &binding : m_config.controller[input])
                if (binding.kind == PadBinding::Kind::Button && binding.code == event.gbutton.button)
                    m_pressed |= kInputs[input].mask;
        return;
    }
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        Gamepad *gamepad = findGamepad(event.gaxis.which);
        if (!gamepad || !gamepadActive(*gamepad) || event.gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT)
            return;
        if (std::abs(static_cast<int>(event.gaxis.value)) > kAxisFull / 2)
            m_controllerAt = SDL_GetTicks();
        // Latch only the crossing, so a trigger resting past the threshold
        // does not repeat its press with every small movement.
        const int threshold = static_cast<int>(m_config.triggerThreshold * kAxisFull);
        for (int direction : {-1, 1}) {
            bool &was = gamepad->axisPressed[event.gaxis.axis * 2u + (direction > 0 ? 1u : 0u)];
            const bool now = event.gaxis.value * direction > threshold;
            if (now && !was)
                for (size_t input = 0; input < kPadInputCount; ++input)
                    for (const auto &binding : m_config.controller[input])
                        if (binding.kind == PadBinding::Kind::Axis && binding.code == event.gaxis.axis &&
                            binding.direction == direction)
                            m_pressed |= kInputs[input].mask;
            was = now;
        }
    }
}

PadSnapshot SdlPadInput::preview() const {
    return sample(SDL_GetKeyboardState(nullptr));
}

PadSnapshot SdlPadInput::sample(const bool *keys) const {
    if (!m_config.keyboardEnabled)
        keys = nullptr;
    const int threshold = static_cast<int>(m_config.triggerThreshold * kAxisFull);
    const SDL_MouseButtonFlags mouse = keys ? SDL_GetMouseState(nullptr, nullptr) : 0u;
    uint32_t held = 0u;
    int stick[kPadInputCount] = {};
    const auto read = [&](SDL_Gamepad *gamepad, const bool *keyState,
                          const std::array<PadBindingSlots, kPadInputCount> &bindings) {
        for (size_t input = 0; input < kPadInputCount; ++input) {
            for (const auto &binding : bindings[input]) {
                const int value = bindingValue(binding, gamepad, keyState, keyState ? mouse : 0u);
                if (kInputs[input].mask != 0u) {
                    if (value > threshold)
                        held |= kInputs[input].mask;
                } else {
                    stick[input] += value;
                }
            }
        }
    };
    for (const auto &gamepad : m_gamepads)
        if (gamepadActive(gamepad) && SDL_GamepadConnected(gamepad.handle))
            read(gamepad.handle, nullptr, m_config.controller);
    if (keys)
        read(nullptr, keys, m_config.keyboard);

    const auto axis = [&](PadInput negative, PadInput positive) {
        return stick[index(positive)] - stick[index(negative)];
    };
    int rightX = axis(PadInput::RightStickLeft, PadInput::RightStickRight);
    int rightY = axis(PadInput::RightStickUp, PadInput::RightStickDown);
    if (m_config.invertCameraX)
        rightX = -rightX;
    if (m_config.invertCameraY)
        rightY = -rightY;
    const int deadZone = static_cast<int>(m_config.stickDeadZone * kAxisFull);
    const uint8_t lx = stickByte(axis(PadInput::LeftStickLeft, PadInput::LeftStickRight), deadZone);
    const uint8_t ly = stickByte(axis(PadInput::LeftStickUp, PadInput::LeftStickDown), deadZone);
    const uint8_t rx = stickByte(rightX, deadZone);
    const uint8_t ry = stickByte(rightY, deadZone);
    return {held, (uint32_t(rx) << 24u) | (uint32_t(ry) << 16u) | (uint32_t(lx) << 8u) | ly,
            stick[index(PadInput::Walk)] > threshold};
}

void SdlPadInput::poll(const bool *keys, bool focused) {
    if (!focused) {
        clearState();
        return;
    }
    const PadSnapshot state = sample(keys);
    uint32_t held = state.held;
    if (m_suppressPending) {
        m_suppressed = held;
        m_suppressPending = false;
    }
    m_suppressed &= held;
    held &= ~m_suppressed;
    m_pressed &= ~m_suppressed;
    m_pressed |= held & ~m_held;
    m_held = held;
    m_sticks = state.sticks;
    m_walking = state.walking;
}

void SdlPadInput::read(uint32_t &held, uint32_t &pressed, uint32_t &sticks) {
    held = m_held;
    pressed = m_pressed;
    sticks = m_sticks;
    m_pressed = 0u;
}
} // namespace dq8::gfx
