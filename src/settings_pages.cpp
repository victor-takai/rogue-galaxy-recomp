#include "settings_pages.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include "free_camera.h"
#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::settingspages
{
namespace
{

constexpr uint32_t kOptionsSetItem = 0x0034F5C0u;
constexpr uint32_t kOptionsRefresh = 0x0034F8E0u;
constexpr uint32_t kOptionsCloseState = 0x0034FC20u;
constexpr uint32_t kOptionsSelectState = 0x0034FE70u;
constexpr uint32_t kOptionsOpenState = 0x0034FF50u;
constexpr uint32_t kOptionLoadValue = 0x00350240u;
constexpr uint32_t kLayout = 0x00482360u;
constexpr uint32_t kGrids = 0x00482364u;
// The column and row MenuOptionsCursorCb last saw.
constexpr uint32_t kLastColumn = 0x00482E84u;
constexpr uint32_t kLastRow = 0x00482E86u;
constexpr uint32_t kDimShade = 0x0047D288u;
constexpr uint16_t kGridTag = 0x1Du;
constexpr uint16_t kLabels = 0x1B4u;
constexpr uint16_t kRowBar = 0x1B5u;
constexpr uint16_t kFirstChoice = 0x1B7u;
constexpr uint16_t kHand = 0x1CBu;
constexpr int kRows = 10;
constexpr int kChoices = kRows * 2;
constexpr int kRowHeight = 30;
// Widget fields: flags (bit 0 hidden), position (and where it is headed),
// colour, sprite and text arrays; a sprite's v; a text's string.
constexpr uint32_t kWidgetFlags = 0x1Cu;
constexpr uint32_t kWidgetPos = 0x14u;
constexpr uint32_t kWidgetPosTarget = 0x60u;
constexpr uint32_t kWidgetColour = 0x30u;
constexpr uint32_t kWidgetSprites = 0x20u;
constexpr uint32_t kWidgetTexts = 0x28u;
constexpr uint32_t kSpriteV = 0xAu;
// A choice draws two sprites (x, y, w, h, u, v, its piece's w and h, ...;
// a list ended by x -1): its button, the same picture for every choice
// (the upper half picked, the lower not), and its word, a piece of the
// words' picture. The game's page has Low and High on row 0, On and Off
// on row 3.
constexpr uint32_t kSpriteBytes = 0x20u;
constexpr int kLowChoice = 0;
constexpr int kOnChoice = 6;
constexpr uint32_t kTextString = 0x4u;
constexpr uint32_t kTextStride = 0x10u;
constexpr int16_t kLowerHalf = 0x1A;
// Pad 1: this frame's buttons, the last frame's, and the lock that makes the
// game read none.
constexpr uint32_t kPad1 = 0x004B2570u;
constexpr uint32_t kPadButtons = 0x4u;
constexpr uint32_t kPadLastButtons = 0x5Cu;
constexpr uint32_t kPadLock = 0x24Cu;
constexpr uint32_t kButtonL1 = 0x4u;
constexpr uint32_t kButtonR1 = 0x8u;
constexpr uint32_t kTextBytes = 48u;

enum class Kind
{
    Switch,
    Number,
};

struct Row
{
    const char *key;
    const char *label;
    Kind kind;
    const char *suffix;
    // A number that names a choice: its value's name in the label.
    const char *const *names = nullptr;
};

// The F1 menu's Display > Borders, in its order.
constexpr const char *kSidesNames[] = {"Black", "Blurred", "Image"};

constexpr Row kSwitches[] = {
    {"widescreen", "Widescreen", Kind::Switch, ""},
    {"skip_opening", "Skip the Opening", Kind::Switch, ""},
    {"instant_loading_recap", "Quick Loading Recap", Kind::Switch, ""},
    {"instant_text", "Instant Text", Kind::Switch, ""},
    {"map_chests", "Chests on Map", Kind::Switch, ""},
    {"save_anywhere", "Save Anywhere R1+Tri", Kind::Switch, ""},
    {"faster_camera", "Faster Camera", Kind::Switch, ""},
    {"free_camera", "Free Camera", Kind::Switch, ""},
    {"dev_tools", "Developer Tools", Kind::Switch, ""},
    {"sixty_fps", "60 FPS (Next Start)", Kind::Switch, ""},
};
constexpr Row kNumbers[] = {
    {"field_of_view", "Field of View", Kind::Number, ""},
    {"camera_distance", "Camera Distance", Kind::Number, "%"},
    {"draw_distance", "Draw Distance", Kind::Number, "%"},
    {"encounter_rate", "Encounters", Kind::Number, "%"},
    {"battle_experience", "Experience", Kind::Number, "%"},
    {"battle_money", "Money", Kind::Number, "%"},
    {"item_drops", "Item Drops", Kind::Number, "%"},
    {"weapon_growth", "Weapon Growth", Kind::Number, "%"},
    {"shop_prices", "Shop Prices", Kind::Number, "%"},
    {"action_gauge", "Action Gauge", Kind::Number, "%"},
};

constexpr Row kScenes[] = {
    {"fast_transporter", "Fast Transporters", Kind::Switch, ""},
    {"fast_revelation", "Fast Revelation", Kind::Switch, ""},
    {"sharp_letters", "Sharp Letters", Kind::Switch, ""},
    {"full_bag_to_storage", "Full Bag to Storage", Kind::Switch, ""},
    {"stack_limit_99", "Item Limit 99", Kind::Switch, ""},
    {"bigger_bag", "Bag Page 7 (Next Start)", Kind::Switch, ""},
    {"sides", "Borders", Kind::Number, "", kSidesNames},
    {"minimap_size", "Minimap Size", Kind::Number, "%"},
    {"compass_size", "Compass Size", Kind::Number, "%"},
};

struct Page
{
    const Row *rows;
    int count;
};
// Page 0 is the game's.
constexpr Page kPages[] = {
    {nullptr, kRows},
    {kSwitches, static_cast<int>(std::size(kSwitches))},
    {kNumbers, static_cast<int>(std::size(kNumbers))},
    {kScenes, static_cast<int>(std::size(kScenes))},
};
constexpr int kPageCount = static_cast<int>(std::size(kPages));
static_assert(std::size(kSwitches) <= kRows && std::size(kNumbers) <= kRows && std::size(kScenes) <= kRows,
              "a page shows kRows rows at most");

struct Range
{
    int minimum = 0;
    int maximum = 0;
    int step = 1;
};

PS2Runtime::RecompiledFunction g_setItem = nullptr;
PS2Runtime::RecompiledFunction g_refresh = nullptr;
PS2Runtime::RecompiledFunction g_closeState = nullptr;
PS2Runtime::RecompiledFunction g_selectState = nullptr;
PS2Runtime::RecompiledFunction g_openState = nullptr;
PS2Runtime::RecompiledFunction g_loadValue = nullptr;

// The F1 menu is up: what the pages show and keep is its.
std::atomic<bool> g_enabled{false};
std::mutex g_valuesMutex;
std::map<std::string, int> g_values;
std::map<std::string, Range> g_ranges;
std::mutex g_changesMutex;
std::deque<Change> g_changes;

// Our strings in guest memory: a row's name each, and an empty one.
uint32_t g_text = 0u;

// The screen's widgets and grid, as found in guest memory.
struct Screen
{
    uint32_t texts = 0u;
    uint32_t rowBar = 0u;
    uint32_t hand = 0u;
    uint32_t grid = 0u;
    uint32_t choices[kChoices] = {};
    uint32_t sprites[kChoices] = {};
};

// The game's page as it was, while one of ours is up (the game thread's
// alone, as is the page).
struct Saved
{
    bool valid = false;
    Screen screen;
    uint32_t strings[kRows] = {};
    uint8_t words[kChoices][kSpriteBytes] = {};
    int16_t flags[kChoices] = {};
    float colours[kChoices][4] = {};
    int16_t v[kChoices] = {};
    int16_t lastRow = 0;
};
Saved g_saved;
int g_page = 0;

template <typename T>
bool readGuest(const uint8_t *rdram, uint32_t address, T &out)
{
    address &= 0x1FFFFFFFu;
    if (address == 0u || address + sizeof(T) > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(&out, rdram + address, sizeof(T));
    return true;
}

template <typename T>
void writeGuest(uint8_t *rdram, uint32_t address, const T &value)
{
    address &= 0x1FFFFFFFu;
    if (address != 0u && address + sizeof(T) <= PS2_RAM_SIZE)
    {
        std::memcpy(rdram + address, &value, sizeof(T));
    }
}

uint32_t findWidget(const uint8_t *rdram, uint32_t layout, uint16_t id)
{
    uint32_t node = 0u;
    if (!readGuest(rdram, layout, node))
    {
        return 0u;
    }
    for (int guard = 0; node != 0u && guard < 4096; ++guard)
    {
        uint32_t widget = 0u;
        uint16_t widgetId = 0u;
        if (!readGuest(rdram, node, widget) || widget == 0u || !readGuest(rdram, widget, widgetId))
        {
            return 0u;
        }
        if (widgetId == id)
        {
            return widget;
        }
        if (!readGuest(rdram, node + 8u, node))
        {
            return 0u;
        }
    }
    return 0u;
}

uint32_t findGrid(const uint8_t *rdram)
{
    uint32_t list = 0u, entry = 0u;
    if (!readGuest(rdram, kGrids, list) || !readGuest(rdram, list + 4u, entry))
    {
        return 0u;
    }
    for (int guard = 0; guard < 256; ++guard, entry += 0x20u)
    {
        uint32_t node = 0u;
        uint16_t tag = 0u;
        if (!readGuest(rdram, entry, node) || node == 0u || !readGuest(rdram, node, tag))
        {
            return 0u;
        }
        if (tag == kGridTag)
        {
            return entry;
        }
    }
    return 0u;
}

bool findScreen(const uint8_t *rdram, Screen &screen)
{
    uint32_t layout = 0u;
    if (!readGuest(rdram, kLayout, layout))
    {
        return false;
    }
    const uint32_t labels = findWidget(rdram, layout, kLabels);
    screen.rowBar = findWidget(rdram, layout, kRowBar);
    screen.hand = findWidget(rdram, layout, kHand);
    screen.grid = findGrid(rdram);
    if (!labels || !screen.rowBar || !screen.hand || !screen.grid ||
        !readGuest(rdram, labels + kWidgetTexts, screen.texts) || screen.texts == 0u)
    {
        return false;
    }
    for (int choice = 0; choice < kChoices; ++choice)
    {
        screen.choices[choice] = findWidget(rdram, layout, static_cast<uint16_t>(kFirstChoice + choice));
        int16_t wordX = -1, endX = 0;
        if (!screen.choices[choice] ||
            !readGuest(rdram, screen.choices[choice] + kWidgetSprites, screen.sprites[choice]) ||
            !readGuest(rdram, screen.sprites[choice] + kSpriteBytes, wordX) ||
            !readGuest(rdram, screen.sprites[choice] + 2u * kSpriteBytes, endX) || wordX == -1 || endX != -1)
        {
            return false;
        }
    }
    return true;
}

bool sameScreen(const Screen &a, const Screen &b)
{
    return a.texts == b.texts && a.grid == b.grid && std::equal(std::begin(a.choices), std::end(a.choices), b.choices);
}

int value(const Row &row)
{
    if (std::strcmp(row.key, "free_camera") == 0)
    {
        return rg::freecamera::enabled() ? 1 : 0;
    }
    std::lock_guard lock(g_valuesMutex);
    const auto found = g_values.find(row.key);
    return found != g_values.end() ? found->second : 0;
}

void change(const Row &row, int newValue)
{
    {
        std::lock_guard lock(g_valuesMutex);
        g_values[row.key] = newValue;
    }
    if (std::strcmp(row.key, "free_camera") == 0)
    {
        // Shown from the free camera itself, so it changes now.
        rg::freecamera::setEnabled(newValue != 0);
    }
    std::lock_guard lock(g_changesMutex);
    g_changes.push_back(Change{row.key, row.kind == Kind::Number, newValue});
}

void writeString(uint8_t *rdram, uint32_t address, const char *text)
{
    char buffer[kTextBytes] = {};
    std::snprintf(buffer, sizeof(buffer), "%s", text);
    std::memcpy(rdram + (address & 0x1FFFFFFFu), buffer, sizeof(buffer));
}

void writeLabel(uint8_t *rdram, const Screen &screen, int index, const Row &row)
{
    const uint32_t string = g_text + static_cast<uint32_t>(index) * kTextBytes;
    char text[kTextBytes];
    if (row.names)
    {
        std::snprintf(text, sizeof(text), "%s: %s", row.label, row.names[value(row)]);
    }
    else if (row.kind == Kind::Number)
    {
        std::snprintf(text, sizeof(text), "%s: %d%s", row.label, value(row), row.suffix);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%s", row.label);
    }
    writeString(rdram, string, text);
    writeGuest(rdram, screen.texts + static_cast<uint32_t>(index) * kTextStride + kTextString, string);
}

void shade(uint8_t *rdram, const Screen &screen, int choice, bool picked)
{
    float dim = 0.6f;
    readGuest(rdram, kDimShade, dim);
    const float level = picked ? 1.0f : dim;
    const float colour[4] = {level, level, level, 1.0f};
    std::memcpy(rdram + (screen.choices[choice] & 0x1FFFFFFFu) + kWidgetColour, colour, sizeof(colour));
    writeGuest(rdram, screen.sprites[choice] + kSpriteV, static_cast<int16_t>(picked ? 0 : kLowerHalf));
}

void highlight(uint8_t *rdram, const Screen &screen, int index, const Row &row)
{
    // A number's Low and High both stand ready.
    const bool on = row.kind == Kind::Number || value(row) != 0;
    const bool off = row.kind == Kind::Number || value(row) == 0;
    shade(rdram, screen, index * 2, on);
    shade(rdram, screen, index * 2 + 1, off);
}

void setHidden(uint8_t *rdram, const Screen &screen, int choice, bool hidden)
{
    int16_t flags = 0;
    if (readGuest(rdram, screen.choices[choice] + kWidgetFlags, flags))
    {
        flags = static_cast<int16_t>(hidden ? (flags | 1) : (flags & ~1));
        writeGuest(rdram, screen.choices[choice] + kWidgetFlags, flags);
    }
}

void save(const uint8_t *rdram, const Screen &screen)
{
    g_saved = Saved{};
    g_saved.screen = screen;
    for (int index = 0; index < kRows; ++index)
    {
        readGuest(rdram, screen.texts + static_cast<uint32_t>(index) * kTextStride + kTextString,
                  g_saved.strings[index]);
    }
    for (int choice = 0; choice < kChoices; ++choice)
    {
        readGuest(rdram, screen.sprites[choice] + kSpriteBytes, g_saved.words[choice]);
        readGuest(rdram, screen.choices[choice] + kWidgetFlags, g_saved.flags[choice]);
        readGuest(rdram, screen.choices[choice] + kWidgetColour, g_saved.colours[choice]);
        readGuest(rdram, screen.sprites[choice] + kSpriteV, g_saved.v[choice]);
    }
    readGuest(rdram, screen.grid + 0xEu, g_saved.lastRow);
    g_saved.valid = true;
}

void restore(uint8_t *rdram, const Screen &screen)
{
    for (int index = 0; index < kRows; ++index)
    {
        writeGuest(rdram, screen.texts + static_cast<uint32_t>(index) * kTextStride + kTextString,
                   g_saved.strings[index]);
    }
    for (int choice = 0; choice < kChoices; ++choice)
    {
        writeGuest(rdram, screen.sprites[choice] + kSpriteBytes, g_saved.words[choice]);
        writeGuest(rdram, screen.choices[choice] + kWidgetFlags, g_saved.flags[choice]);
        writeGuest(rdram, screen.choices[choice] + kWidgetColour, g_saved.colours[choice]);
        writeGuest(rdram, screen.sprites[choice] + kSpriteV, g_saved.v[choice]);
    }
    writeGuest(rdram, screen.grid + 0xEu, g_saved.lastRow);
}

// The game's word for `from` (a choice of its page) on choice `to`: its
// piece of the words' picture, placed on `to`'s button as on `from`'s.
void copyWord(uint8_t *rdram, const Screen &screen, int from, int to)
{
    int16_t word[kSpriteBytes / 2];
    std::memcpy(word, g_saved.words[from], sizeof(word));
    int16_t fromButton[2] = {}, toButton[2] = {};
    readGuest(rdram, g_saved.screen.sprites[from], fromButton);
    readGuest(rdram, screen.sprites[to], toButton);
    word[0] = static_cast<int16_t>(toButton[0] + word[0] - fromButton[0]);
    word[1] = static_cast<int16_t>(toButton[1] + word[1] - fromButton[1]);
    writeGuest(rdram, screen.sprites[to] + kSpriteBytes, word);
}

void showPage(uint8_t *rdram, const Screen &screen, int page)
{
    const Page &shown = kPages[page];
    const uint32_t empty = g_text + kRows * kTextBytes;
    for (int index = 0; index < kRows; ++index)
    {
        if (index >= shown.count)
        {
            writeGuest(rdram, screen.texts + static_cast<uint32_t>(index) * kTextStride + kTextString, empty);
            setHidden(rdram, screen, index * 2, true);
            setHidden(rdram, screen, index * 2 + 1, true);
            continue;
        }
        const Row &row = shown.rows[index];
        const bool number = row.kind == Kind::Number;
        writeLabel(rdram, screen, index, row);
        copyWord(rdram, screen, number ? kLowChoice : kOnChoice, index * 2);
        copyWord(rdram, screen, number ? kLowChoice + 1 : kOnChoice + 1, index * 2 + 1);
        setHidden(rdram, screen, index * 2, false);
        setHidden(rdram, screen, index * 2 + 1, false);
        highlight(rdram, screen, index, row);
    }
    writeGuest(rdram, screen.grid + 0xEu, static_cast<int16_t>(shown.count - 1));
}

void placeCursor(uint8_t *rdram, const Screen &screen, int16_t column, int16_t row)
{
    int16_t grid[0x10] = {};
    std::memcpy(grid, rdram + (screen.grid & 0x1FFFFFFFu), sizeof(grid));
    writeGuest(rdram, screen.grid + 0x14u, column);
    writeGuest(rdram, screen.grid + 0x16u, row);
    writeGuest(rdram, kLastColumn, column);
    writeGuest(rdram, kLastRow, row);
    // Where MenuOptionsOpenState puts them: grid +4/+6 the first cell, +8/+A
    // a cell's size.
    const float hand[2] = {static_cast<float>(grid[2] + column * grid[4]), static_cast<float>(grid[3] + row * grid[5])};
    const float bar[2] = {0.0f, static_cast<float>(row * kRowHeight)};
    std::memcpy(rdram + (screen.hand & 0x1FFFFFFFu) + kWidgetPos, hand, sizeof(hand));
    std::memcpy(rdram + (screen.hand & 0x1FFFFFFFu) + kWidgetPosTarget, hand, sizeof(hand));
    std::memcpy(rdram + (screen.rowBar & 0x1FFFFFFFu) + kWidgetPos, bar, sizeof(bar));
    std::memcpy(rdram + (screen.rowBar & 0x1FFFFFFFu) + kWidgetPosTarget, bar, sizeof(bar));
}

// The column MenuOptionLoadValue would put the cursor in on a row: a
// switch's setting; elsewhere where it is.
int16_t settingColumn(int page, int16_t row, int16_t column)
{
    const Page &shown = kPages[page];
    if (row < 0 || row >= shown.count || shown.rows[row].kind != Kind::Switch)
    {
        return column;
    }
    return value(shown.rows[row]) != 0 ? 0 : 1;
}

void turnPage(uint8_t *rdram, int direction)
{
    Screen screen;
    if (!findScreen(rdram, screen) || g_text == 0u)
    {
        return;
    }
    if (g_page == 0)
    {
        save(rdram, screen);
    }
    else if (!g_saved.valid || !sameScreen(g_saved.screen, screen))
    {
        // Not the screen we changed: leave it to the game.
        g_page = 0;
        g_saved.valid = false;
        return;
    }
    g_page = (g_page + direction + kPageCount) % kPageCount;
    if (g_page == 0)
    {
        restore(rdram, screen);
        g_saved.valid = false;
    }
    else
    {
        showPage(rdram, screen, g_page);
    }
    int16_t column = 0, row = 0;
    readGuest(rdram, screen.grid + 0x14u, column);
    readGuest(rdram, screen.grid + 0x16u, row);
    row = std::min<int16_t>(row, static_cast<int16_t>(kPages[g_page].count - 1));
    if (g_page != 0)
    {
        column = settingColumn(g_page, row, column);
    }
    else
    {
        // On the game's page, the picked one: its button's upper half.
        int16_t v = 0;
        readGuest(rdram, screen.sprites[row * 2] + kSpriteV, v);
        column = v == 0 ? 0 : 1;
    }
    placeCursor(rdram, screen, column, row);
}

// Back to the game's page, if one of ours is up on this screen.
void backToGame(uint8_t *rdram)
{
    if (g_page != 0 && g_saved.valid)
    {
        Screen screen;
        if (findScreen(rdram, screen) && sameScreen(g_saved.screen, screen))
        {
            restore(rdram, screen);
        }
    }
    g_page = 0;
    g_saved.valid = false;
}

void returnOne(R5900Context *ctx)
{
    setReturnS32(ctx, 1);
    ctx->pc = getRegU32(ctx, 31);
}

void selectState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    uint32_t buttons = 0u, last = 0u, locked = 0u;
    readGuest(rdram, kPad1 + kPadButtons, buttons);
    readGuest(rdram, kPad1 + kPadLastButtons, last);
    readGuest(rdram, kPad1 + kPadLock, locked);
    // Without the F1 menu the screen stays the game's: nothing would read
    // the options' values in, or take a change out.
    const uint32_t pressed = locked != 0u || !g_enabled.load(std::memory_order_relaxed) ? 0u : buttons & ~last;
    if (pressed & kButtonR1)
    {
        turnPage(rdram, 1);
    }
    else if (pressed & kButtonL1)
    {
        turnPage(rdram, -1);
    }
    g_selectState(rdram, ctx, runtime);
}

void setItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_page == 0)
    {
        g_setItem(rdram, ctx, runtime);
        return;
    }
    const int column = static_cast<int>(getRegU32(ctx, 4));
    const int index = static_cast<int>(getRegU32(ctx, 5));
    const Page &shown = kPages[g_page];
    Screen screen;
    if (index >= 0 && index < shown.count && findScreen(rdram, screen))
    {
        const Row &row = shown.rows[index];
        if (row.kind == Kind::Switch)
        {
            change(row, column == 0 ? 1 : 0);
        }
        else
        {
            Range range;
            {
                std::lock_guard lock(g_valuesMutex);
                range = g_ranges[row.key];
            }
            const int next = std::clamp(value(row) + (column == 0 ? -range.step : range.step), range.minimum,
                                        range.maximum);
            if (next != value(row))
            {
                change(row, next);
            }
            writeLabel(rdram, screen, index, row);
        }
        highlight(rdram, screen, index, row);
    }
    returnOne(ctx);
}

void loadValue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_page == 0)
    {
        g_loadValue(rdram, ctx, runtime);
        return;
    }
    // Its argument is the grid entry's +4: +0x10 the column, +0x12 the row.
    const uint32_t cursor = getRegU32(ctx, 4);
    int16_t column = 0, row = 0;
    if (readGuest(rdram, cursor + 0x10u, column) && readGuest(rdram, cursor + 0x12u, row))
    {
        writeGuest(rdram, cursor + 0x10u, settingColumn(g_page, row, column));
    }
    returnOne(ctx);
}

void refresh(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_page == 0)
    {
        g_refresh(rdram, ctx, runtime);
        return;
    }
    Screen screen;
    if (findScreen(rdram, screen))
    {
        const Page &shown = kPages[g_page];
        for (int index = 0; index < shown.count; ++index)
        {
            highlight(rdram, screen, index, shown.rows[index]);
        }
    }
    returnOne(ctx);
}

void openState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 5) != 0u)
    {
        backToGame(rdram);
    }
    g_openState(rdram, ctx, runtime);
}

void closeState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    backToGame(rdram);
    g_closeState(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    g_text = runtime.guestCalloc(kRows + 1, kTextBytes);
    const bool complete =
        g_text != 0u && rg::timing::hookFunction(runtime, kOptionsSetItem, &setItem, g_setItem) &&
        rg::timing::hookFunction(runtime, kOptionsRefresh, &refresh, g_refresh) &&
        rg::timing::hookFunction(runtime, kOptionsCloseState, &closeState, g_closeState) &&
        rg::timing::hookFunction(runtime, kOptionsSelectState, &selectState, g_selectState) &&
        rg::timing::hookFunction(runtime, kOptionsOpenState, &openState, g_openState) &&
        rg::timing::hookFunction(runtime, kOptionLoadValue, &loadValue, g_loadValue);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] settings pages: some hooks are missing, see above\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

void noteOption(const std::string &key, bool on)
{
    std::lock_guard lock(g_valuesMutex);
    g_values[key] = on ? 1 : 0;
}

void noteNumber(const std::string &key, int value)
{
    std::lock_guard lock(g_valuesMutex);
    g_values[key] = value;
}

void setRange(const std::string &key, int minimum, int maximum, int step, int defaultValue)
{
    std::lock_guard lock(g_valuesMutex);
    g_ranges[key] = Range{minimum, maximum, std::max(step, 1)};
    g_values.emplace(key, defaultValue);
}

bool takeChange(Change &change)
{
    std::lock_guard lock(g_changesMutex);
    if (g_changes.empty())
    {
        return false;
    }
    change = std::move(g_changes.front());
    g_changes.pop_front();
    return true;
}

} // namespace rg::settingspages
