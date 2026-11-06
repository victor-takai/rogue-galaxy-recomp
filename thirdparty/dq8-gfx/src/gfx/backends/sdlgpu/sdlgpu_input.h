#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dq8::gfx {

// The PS2 pad's inputs, in the order the settings list them.
enum class PadInput : uint8_t {
    Cross, Circle, Square, Triangle,
    Up, Down, Left, Right,
    L1, R1, L2, R2, L3, R3,
    Start, Select,
    LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
    RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
    // Not the pad's: held, the left stick is pushed only part of the way, a
    // walk (keys push it all the way).
    Walk,
    Count
};
constexpr size_t kPadInputCount = static_cast<size_t>(PadInput::Count);

// One host input: a key, a gamepad button, one direction of a gamepad axis, a
// mouse button, or a turn of the mouse wheel. The mouse's go in the keyboard's
// slots; a wheel turn is a press, never held.
struct PadBinding {
    enum class Kind : uint8_t { None, Key, Button, Axis, Mouse, Wheel };
    Kind kind = Kind::None;
    // SDL_Scancode, SDL_GamepadButton, SDL_GamepadAxis or SDL's mouse button
    // (SDL_BUTTON_LEFT to SDL_BUTTON_X2); a wheel's is 0.
    int16_t code = 0;
    int8_t direction = 1; // an axis's, or the wheel's: 1 away from you (up), -1 toward

    static PadBinding key(SDL_Scancode scancode);
    static PadBinding button(SDL_GamepadButton button);
    static PadBinding axis(SDL_GamepadAxis axis, int direction);
    static PadBinding mouse(int button);
    static PadBinding wheel(int direction);
    bool bound() const { return kind != Kind::None; }
    bool operator==(const PadBinding &) const = default;
};

constexpr size_t kBindingSlots = 2u;
using PadBindingSlots = std::array<PadBinding, kBindingSlots>;

enum class ControllerSelection : uint8_t { Any, One, None };

struct PadConfig {
    std::array<PadBindingSlots, kPadInputCount> controller{};
    std::array<PadBindingSlots, kPadInputCount> keyboard{};
    bool keyboardEnabled = true;
    ControllerSelection controllers = ControllerSelection::Any;
    // ControllerSelection::One: the model, and the serial where the device
    // reports one; the name is shown while it is disconnected.
    std::string controllerGuid;
    std::string controllerSerial;
    std::string controllerName;
    // Not saved: the device picked this session, which tells identical
    // controllers without a serial apart until it disconnects.
    SDL_JoystickID controllerInstance = 0;
    float stickDeadZone = 0.125f;
    float triggerThreshold = 0.25f;
    bool invertCameraX = false;
    bool invertCameraY = false;

    // Which keyboard defaults a settings file was written with: before 2 (no
    // `layout` in [keyboard]), the emulator-like keys (X, C, Z, V...). A file
    // whose keys are still all those gets the new ones.
    static constexpr int kKeyboardLayout = 2;
    int keyboardLayout = kKeyboardLayout;

    static PadConfig defaults();
    // The keyboard's defaults of layout 1, Walk unbound.
    static std::array<PadBindingSlots, kPadInputCount> firstKeyboardLayout();
    PadBindingSlots &slots(PadInput input, bool keyboardSlots) {
        return (keyboardSlots ? keyboard : controller)[static_cast<size_t>(input)];
    }
    const PadBindingSlots &slots(PadInput input, bool keyboardSlots) const {
        return (keyboardSlots ? keyboard : controller)[static_cast<size_t>(input)];
    }
};

struct ControllerInfo {
    SDL_JoystickID id = 0;
    std::string name;
    std::string guid;
    std::string serial; // empty when the device reports none
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
    bool active = false; // drives the game under the current selection
    SDL_PowerState power = SDL_POWERSTATE_UNKNOWN;
    int batteryPercent = -1;
};

// The pad as the game last saw it.
struct PadSnapshot {
    uint32_t held = 0u;
    uint32_t sticks = 0x80808080u;
    bool walking = false; // PadInput::Walk held
};

// The pad report's bit for a button; 0 for a stick direction.
uint32_t padButtonMask(PadInput input);
const char *padInputName(PadInput input);
// Stable lower-case names for settings files.
const char *padInputKey(PadInput input);
std::string padBindingToString(const PadBinding &binding);
// "none" is the empty binding; text that names no input gives nothing.
std::optional<PadBinding> padBindingFromString(const std::string &text);

// Main-thread SDL input, published in the PS2 pad report's button/axis encoding.
class SdlPadInput {
public:
    SdlPadInput() = default;
    SdlPadInput(const SdlPadInput &) = delete;
    SdlPadInput &operator=(const SdlPadInput &) = delete;
    ~SdlPadInput();
    bool initialize(std::string &error);
    void handleEvent(const SDL_Event &event);
    void poll(const bool *keys, bool focused);
    void read(uint32_t &held, uint32_t &pressed, uint32_t &sticks);

    void setConfig(const PadConfig &config);
    const PadConfig &config() const { return m_config; }
    std::vector<ControllerInfo> controllers() const;
    PadSnapshot snapshot() const { return {m_held, m_sticks, m_walking}; }
    // Whether PadInput::Walk is held, as of the last poll.
    bool walking() const { return m_walking; }
    // SDL_GetTicks() of the last press of a key or mouse button bound to the
    // pad (or a wheel turn), of the last mouse motion, and of the last
    // controller button or push past half a stick: which the player uses.
    uint64_t keyboardMouseAt() const { return m_keyboardMouseAt; }
    uint64_t mouseMotionAt() const { return m_mouseMotionAt; }
    uint64_t controllerAt() const { return m_controllerAt; }
    // In the game's own menus (the host says when, before the events): the
    // right click is Circle, back, and the wheel D-pad up and down, whatever
    // they are bound to. Mouse buttons held when this changes count as
    // released until they are let go, so a guard held as a menu opens does
    // not close it.
    void setMenuMouse(bool on);
    // The pad as the bindings read the devices now, even while the menu
    // keeps it from the game; for a live preview of the bindings.
    PadSnapshot preview() const;
    // Buttons held now stay released for the game until they are let go, so a
    // button that closed the menu does not reach the game as a fresh press.
    void suppressHeld();

private:
    struct Gamepad {
        SDL_Gamepad *handle = nullptr;
        std::string guid;
        std::string serial;
        std::array<bool, SDL_GAMEPAD_AXIS_COUNT * 2> axisPressed{};
    };
    PadSnapshot sample(const bool *keys, SDL_MouseButtonFlags mouse) const;
    void openGamepad(SDL_JoystickID id);
    void openAll();
    void clearState();
    bool gamepadActive(const Gamepad &gamepad) const;
    // The one that plays under ControllerSelection::One, if connected.
    const Gamepad *chosenGamepad() const;
    Gamepad *findGamepad(SDL_JoystickID id);

    PadConfig m_config = PadConfig::defaults();
    std::vector<Gamepad> m_gamepads;
    bool m_initialized = false;
    uint32_t m_held = 0u;
    uint32_t m_pressed = 0u;
    uint32_t m_suppressed = 0u;
    bool m_suppressPending = false;
    uint32_t m_sticks = 0x80808080u;
    bool m_walking = false;
    bool m_menuMouse = false;
    SDL_MouseButtonFlags m_mouseButtons = 0u;
    SDL_MouseButtonFlags m_mouseSuppressed = 0u;
    uint64_t m_keyboardMouseAt = 0u;
    uint64_t m_mouseMotionAt = 0u;
    uint64_t m_controllerAt = 0u;
};

} // namespace dq8::gfx
