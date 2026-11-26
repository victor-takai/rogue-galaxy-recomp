// The Insectron screens in the European languages: the capture notices (main
// executable), the Insectron battle (the overlay INSECT.BIN) and the Insectron
// Vs. menus (INSMENU.BIN). The European executable sizes their windows per
// language from tables, where the US code has the English sizes as constants.
// Here the calls the US code makes to the layout functions get the size of the
// language the text is in, told by where they return (and, from an overlay,
// by that overlay being resident). Where the PAL code changed a layout for
// every language (the rows' text a little lower, a help box sized to its
// words), the US one stays. The cage's commands and the name entry are in
// text_language.cpp.
#include "text_language.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>

#include "overlay/overlay_manager.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::textlanguage
{
namespace
{
constexpr int kV0 = 2;
constexpr int kA0 = 4;
constexpr int kA1 = 5;
constexpr int kA2 = 6;
constexpr int kT2 = 10;
constexpr int kS0 = 16;
constexpr int kS1 = 17;
constexpr int kS4 = 20;
constexpr int kRa = 31;

constexpr uint32_t kWidgetSprites = 0x20u;
constexpr uint32_t kWidgetTexts = 0x28u;
constexpr uint32_t kSpriteBytes = 0x20u;
constexpr uint32_t kSpriteWidth = 0x4u;
constexpr uint32_t kTextBytes = 0x10u;

uint32_t readWord(const uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    return value;
}

int16_t readHalf(const uint8_t *rdram, uint32_t address)
{
    int16_t value = 0;
    std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    return value;
}

void writeHalf(uint8_t *rdram, uint32_t address, int16_t value)
{
    std::memcpy(rdram + (address & 0x1FFFFFFFu), &value, sizeof(value));
}

int32_t reg(const R5900Context *ctx, int r)
{
    return static_cast<int32_t>(getRegU32(ctx, r));
}

void setReg(R5900Context *ctx, int r, int32_t value)
{
    ctx->r[r] = _mm_set_epi64x(0, static_cast<int64_t>(value));
}

// Calls `visit(widget, id)` for each widget of the UI at `ui`, in its list's
// order, until it returns true. The same walk as text_language.cpp's
// findWidget.
template <typename Visit>
void forEachWidget(const uint8_t *rdram, uint32_t ui, Visit visit)
{
    uint32_t node = readWord(rdram, ui);
    for (int guard = 0; node != 0u && guard < 4096; ++guard)
    {
        const uint32_t widget = readWord(rdram, node);
        if (widget == 0u || visit(widget, static_cast<uint16_t>(readHalf(rdram, widget))))
        {
            return;
        }
        node = readWord(rdram, node + 8u);
    }
}

uint32_t findWidget(const uint8_t *rdram, uint32_t ui, uint16_t id)
{
    uint32_t found = 0u;
    forEachWidget(rdram, ui, [&](uint32_t widget, uint16_t widgetId) {
        found = widgetId == id ? widget : 0u;
        return found != 0u;
    });
    return found;
}

// UiSetSpriteW's write: sprite `index` of the widget `width` wide, when it is
// `us` wide (the US layout's), so that a layout other than the one measured
// (fewer sprites, other sizes) is never written past.
void setSpriteWidth(uint8_t *rdram, uint32_t ui, uint32_t id, int index, int32_t us, int32_t width)
{
    const uint32_t widget = findWidget(rdram, ui, static_cast<uint16_t>(id));
    const uint32_t sprites = widget ? readWord(rdram, widget + kWidgetSprites) : 0u;
    const uint32_t at = sprites + static_cast<uint32_t>(index) * kSpriteBytes + kSpriteWidth;
    if (sprites != 0u && readHalf(rdram, at) == us)
    {
        writeHalf(rdram, at, static_cast<int16_t>(width));
    }
}

// Where a call comes from: the main executable, or an overlay (which has to
// be resident: other code stands at those addresses otherwise). The battle's
// help list, its topics' titles, the Vs. battles' statistics and the words
// the PAL code fills in (in INSECT) and the Vs. menus' title banner (in
// INSMENU) have hooks of their own, so that one of them failing leaves the
// rest alone.
enum class From : uint8_t
{
    Main,
    Insect,
    Insmenu,
    Help,
    Banner,
    HelpTitles,
    Stats,
    Words,
    Count,
};

// Each screen's hooks went in (installInsectronLayouts), by From.
bool g_screenOn[static_cast<int>(From::Count)] = {};

// The European column (French 0, German 1, Spanish 2, Italian 3) for a call
// made from `from`, or -1 (English, that screen's hooks did not all go in, or
// that overlay is not the resident one).
int columnFor(From from)
{
    const int column = europeanColumn();
    if (column < 0 || !g_screenOn[static_cast<int>(from)])
    {
        return -1;
    }
    if (from == From::Main)
    {
        return column;
    }
    const char *name = from == From::Insmenu || from == From::Banner ? "insmenu" : "insect";
    return std::strcmp(rg::overlay::residentName(), name) == 0 ? column : -1;
}

// A call whose argument the language changes: the call made from `from`
// returning to `ret` passes `us` in register `reg`; in the language of column
// c it passes `value[c]`.
struct ArgPatch
{
    From from;
    uint32_t ret;
    int reg;
    int32_t us;
    int32_t value[4];
};

constexpr ArgPatch narrower(From from, uint32_t ret, int r, int32_t us, const int32_t (&width)[4], int32_t less)
{
    return {from, ret, r, us - less, {width[0] - less, width[1] - less, width[2] - less, width[3] - less}};
}

constexpr ArgPatch same(From from, uint32_t ret, int r, int32_t us, const int32_t (&width)[4])
{
    return narrower(from, ret, r, us, width, 0);
}

constexpr ArgPatch wider(From from, uint32_t ret, int r, int32_t us, const int32_t (&more)[4])
{
    return {from, ret, r, us, {us + more[0], us + more[1], us + more[2], us + more[3]}};
}

// An x that keeps a right edge: `right` less the width.
constexpr ArgPatch leftOf(From from, uint32_t ret, int r, int32_t right, int32_t usWidth, const int32_t (&width)[4])
{
    return {from, ret, r, right - usWidth, {right - width[0], right - width[1], right - width[2], right - width[3]}};
}

// Applies the patch for this call, if one matches: the return address first,
// as these functions are called all the time from everywhere.
template <size_t N>
void applyPatches(R5900Context *ctx, const ArgPatch (&patches)[N])
{
    const uint32_t from = getRegU32(ctx, kRa);
    for (const ArgPatch &patch : patches)
    {
        if (patch.ret != from || reg(ctx, patch.reg) != patch.us)
        {
            continue;
        }
        const int column = columnFor(patch.from);
        if (column >= 0)
        {
            setReg(ctx, patch.reg, patch.value[column]);
        }
        return;
    }
}

// --- The capture notices (main executable) -------------------------------
// The choice window that opens on checking a placed Insector Trap (the insect
// menu's UI at 0x4E95C0). With an insect in it, "Collect / Set Free"
// (InsectMenuCaughtChoiceOpen, 0x396B60, and the window's layout,
// InsectMenuTopWindowLayout, 0x396EF0); empty, "Collect / Change Location"
// (InsectMenuEmptyCageChoiceOpen, 0x397370, and InsectMenuChoiceWindowLayout,
// 0x396FC0). Each lays out the frame (widget 0x7533) a width wide
// (UiLayoutWindowFrame, a1) at an x (UiSetWidgetPos, a2), and the highlight
// (0x7532) 12 narrower (UiSetSpriteW, a2) at an x. The US widths are
// English's, 94 and 140; the PAL executable takes them from tables per
// language (0x471F58 and 0x471F48: the longer choice in the system font and
// some room) and moves the window so that its right edge and the highlight's
// stay put (348 and 339; 385 and 376). German "Einsammeln" reached the US
// window's edge; French "Ramasser / Déplacer" had 50 to spare.
constexpr int32_t kCaught[4] = {89, 100, 77, 77};
constexpr int32_t kEmpty[4] = {90, 115, 140, 118};
constexpr int32_t kNoticeInset = 12;

// --- The battle (INSECT.BIN) ---------------------------------------------
// The PAL tables (in the main executable, filled at boot from 0x4770D8 and
// 0x477158) run English, Spanish, French, Italian, German; only Spanish and
// German differ from English:
// - the command menus (InsectCmdMenuLayout, 0x4FAB80: a piece's commands
//   Move / Special Move / Parameters / Help / End Turn, the system commands)
//   are 116 wide in English, French and Italian, 122 in Spanish and 126 in
//   German (0x477108): Spanish "Finalizar turno" ran past the US frame,
//   German "Runde beenden" reached its edge;
// - the team panels of the battle's start (InsectBattleMenuTeamSelect,
//   0x5004F0, and its confirmation, 0x5001E0: "Participating Insectors" and
//   the help beside it) are 10 wider in German (0x477188 and the tables after
//   it): "Teilnehmende Insektoiden" ran past its panel.
constexpr int32_t kCommandWidth[4] = {116, 126, 122, 116};
constexpr int32_t kCommandWidthUs = 116;
constexpr int32_t kPanelWider[4] = {0, 10, 0, 0};

// InsectCmdMenuLayout: the frame and its rows through InsectBattleMenu-
// FrameLayoutMaybe (t2 -1: the default width, 116; the PAL one takes it from
// its table, for its one call with -1, returning to 0x4FAC58), the highlight 4
// narrower (UiQuadSetByModeRelated,
// a2 112), and the header above (InsectBattleMenuFrameNineSliceMaybe, a2: the
// title's width, at least 116). In mode 2 the header stands at the screen's
// right and the frame under its right end: the frame's x (UiSetWidgetPos of
// 0x7560) is how much wider than the frame the header is, the header's x
// (0x7564) the screen's width less its width. At these calls s1 holds the
// header's US width and s4 the mode.
constexpr uint32_t kCommandFrameReturn = 0x004FAC58u;
constexpr uint32_t kCommandHighlightReturn = 0x004FAC80u;
constexpr int32_t kCommandHighlightUs = 0x70;
constexpr uint32_t kCommandHeaderReturn = 0x004FAD1Cu;
constexpr uint32_t kCommandFrameAtReturn = 0x004FAD90u;
constexpr uint32_t kCommandHeaderAtReturn = 0x004FADA4u;
constexpr uint32_t kCommandFrame = 0x7560u;
constexpr uint32_t kCommandHeader = 0x7564u;
constexpr int32_t kModeRight = 2;
// InsectBattleMenuSpecialList: the list of special moves slides out from
// under the commands' right edge, 112 (the frame's width less 4, s0) past
// the commands' x (UiMoveWidgetUniform of 0x7580).
constexpr uint32_t kSpecialSlideReturn = 0x004FC490u;
constexpr uint32_t kSpecialList = 0x7580u;
constexpr int32_t kSpecialSlideUs = 0x70;

// InsectBattleMenuTeamSelect, its setup: the "Participating Insectors" panel
// (0x7544, a2 192), the help box beside it (0x7556 at x 200 and 192 wide, its
// fill 0x7557 188), the bar 0x7542 (UiQuadSetByModeRelated, a2 184), the
// panel group sliding in from x -192 (0x753F) and its title centred in 192
// (UiSetTextX of 0x7540). The PAL one then sets the panel's pieces' widths
// (UiSetSpriteW: 0x7541, 0x7540 and 0x7542 184, the five rows 0x7543 188), all
// from its tables. InsectBattleMenuTeamConfirm slides the panels (0x753E) to
// the screen's width less 228 (the panel and 36).
constexpr uint32_t kPanelReturn = 0x005006B0u;
constexpr uint32_t kHelpFillReturn = 0x00500760u;
constexpr uint32_t kTitleXReturn = 0x00500A78u;
constexpr uint32_t kConfirmSlideReturn = 0x005003F0u;
constexpr int32_t kPanelUs = 0xC0;
constexpr float kHelpFillUs = 188.0f;
constexpr uint32_t kTitle = 0x7540u;
constexpr uint32_t kPanels = 0x753Eu;
constexpr uint32_t kTeamUiOffset = 0x878u; // the overlay's UI, at its state's +0x878 (s1 there)
constexpr uint32_t kPieceWidgets[] = {0x7541u, 0x7540u, 0x7542u};
constexpr int32_t kPieceWidthUs = 184;
constexpr uint32_t kRows = 0x7543u;
constexpr int kRowCount = 5;
constexpr int32_t kRowWidthUs = 188;

// InsectBattleHelpStep (0x50A9F0), its list of help topics as it opens: a
// window 288 wide (0x75FC, UiNineSliceSpritesLayout a2) centred on the screen
// (UiSetWidgetPos, the screen's width less 288, halved), the list's back
// (0x75F9) and highlight (0x75FA) 284 (UiQuadSetByModeRelated a2), the rows
// (0x75FB) 284 (UiSpriteListFillColumnMaybe f12), the scroll bar (0x75FD) at x
// 276 and the title centred in 288 (InsectMsgCenterX a2). The PAL code keeps
// these in every language, and its own text shortens some long topics ("Vigila
// el estado de tu insector..."); Spanish "Familia de los escarabajos cuchillo"
// ran over the scroll bar and "...bazuka" reached it; French "Attention à
// l'état de votre Insector..." (whole once its title has the PAL's room,
// below) ran past the window. A measure of ours, not the PAL's: the window 20
// wider in Spanish and 36 in French, which leaves "cuchillo" and "Insector..."
// the room the PAL's shortened topics have (some 13 pixels before the rows'
// end).
constexpr int32_t kHelpWider[4] = {36, 0, 20, 0};
static_assert(kHelpWider[0] % 2 == 0 && kHelpWider[1] % 2 == 0 && kHelpWider[2] % 2 == 0 && kHelpWider[3] % 2 == 0,
              "the window stays centred by moving it half its extra width");
constexpr int32_t kHelpWindowUs = 288;
constexpr int32_t kHelpListUs = 284;
constexpr float kHelpRowsUs = 284.0f;
constexpr int32_t kHelpScrollXUs = 276;
constexpr uint32_t kHelpWindow = 0x75FCu;
constexpr uint32_t kHelpWindowReturn = 0x0050AC9Cu;  // UiNineSliceSpritesLayout
constexpr uint32_t kHelpRowsReturn = 0x0050ACF4u;    // UiSpriteListFillColumnMaybe
constexpr uint32_t kHelpWindowAtReturn = 0x0050ADBCu; // UiSetWidgetPos of 0x75FC

// The help's topics: InsectHelpEntrySetTitle (0x510E80) copies each title
// into 3 quadwords of the help's memory (MemStackAllocQw, a1, returning to
// 0x510EA4), the PAL one into 4 (its only change there; the memory, 0x280
// quadwords, the same). The French title of the topic on an Insector's
// status takes more than 48 bytes: its end ran into the next allocation, and
// the list showed "Insect?/hb0" from the file name written there. The 4
// quadwords in every European language, as the PAL has; English keeps the US
// 3.
constexpr int32_t kHelpTitleQwUs = 3;
constexpr int32_t kHelpTitleQw[4] = {4, 4, 4, 4};
constexpr uint32_t kHelpTitleReturn = 0x00510EA4u;

// The statistics under a Vs. battle's rematch prompt (InsectBattleRematch-
// PromptDraw, 0x4F14E0): per player, 11 texts of the battle's layout
// (Insectron_menu_def: player, label, then number and label three times,
// "Win percentage", number, "%"; rows at y 368 and 400), each drawn through
// InsectBattleDrawTextRec (0x4F1410: a1 the record, +0 x, +2 y, +8 the
// alignment from bit 7), which only this screen calls. The PAL layout places
// them per language; there its code only formats the numbers otherwise
// (right-aligned to 3 digits, Spanish's left-aligned and plain, as the US
// ones). Each x is the PAL language's against PAL English's (which has the
// percentage's label 4 further right), on the US x; Spanish puts each label
// before its number, the numbers left-aligned. Italian "sconfitta(e)" ran
// into "Rapporto".
constexpr int kStatsTexts = 11;
constexpr int16_t kStatsRowY[2] = {368, 400};
constexpr uint32_t kRecordY = 0x2u;
constexpr uint32_t kRecordAlign = 0x8u;
constexpr int kAlignShift = 7;
constexpr int16_t kStatsXUs[kStatsTexts] = {10, 30, 40, 80, 148, 188, 233, 273, 350, 445, 485};
constexpr int16_t kStatsAlignUs[kStatsTexts] = {16, 0, 36, 0, 36, 0, 36, 0, 0, 36, 0};
constexpr int16_t kStatsX[4][kStatsTexts] = {
    {23, 20, 37, 77, 142, 182, 251, 291, 368, 447, 487},  // French
    {23, 20, 43, 83, 152, 192, 242, 282, 383, 445, 485},  // German
    {23, 20, 113, 54, 225, 151, 336, 264, 372, 445, 485}, // Spanish
    {24, 20, 40, 80, 148, 188, 257, 297, 388, 445, 485},  // Italian
};
constexpr int16_t kStatsAlign[4][kStatsTexts] = {
    {16, 0, 36, 0, 36, 0, 36, 0, 0, 36, 0},
    {16, 0, 36, 0, 36, 0, 36, 0, 0, 36, 0},
    {16, 0, 0, 0, 0, 0, 0, 0, 0, 36, 0},
    {16, 0, 36, 0, 36, 0, 36, 0, 0, 36, 0},
};

// A record is moved while it holds its US place, so it is moved once: no
// language's place may be another text's US place.
constexpr bool statsMovedOnce()
{
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < kStatsTexts; ++k)
            for (int j = 0; j < kStatsTexts; ++j)
                if (j != k && kStatsX[c][k] == kStatsXUs[j] && kStatsAlign[c][k] == kStatsAlignUs[j])
                    return false;
    return true;
}
static_assert(statsMovedOnce(), "a statistics text would be moved again as another");

// The words the US battle has fixed: in its layout (insect_status_view):
// "Yes" and "No" on the prompts' buttons (widgets 0xC37B and 0xC37C), " Useful
// Info" (0xC36E's second text) and the opponent panel's "Name:" (0xC382's
// first); in the overlay: "ACE" and "Lv%d" (0x524C28, 0x524C30: a piece's
// level) and "(AI)" (0x524BA8, after a victory code). The PAL layout leaves
// those texts empty, and its InsectBattleLoadResources (0x4F9370), once the
// battle's messages are in (InsectMsgSetBank, here returning to 0x4F969C),
// sets them: the system messages 0x52 and 0x53 (the active table's, which in
// a Vs. battle is the field's) centred at 26 in their buttons, the battle's
// messages 5004 and 5005 (and 5002, ":" in every language); its code takes
// 5000, 5001 and 5003 where the US one has the fixed strings. The European
// texts carry those messages (the PAL file is their base), so the same is
// done here after that call, in a European language; an overlay string takes
// its message where it fits the string's room and the game's use of it.
constexpr uint32_t kMsgSetBankReturn = 0x004F969Cu;
constexpr uint32_t kBattleMessages = 0x506Cu; // the battle's message bank, at its state's +0x506C
constexpr uint32_t kBattleUi = 0x87Cu;        // the battle's UI, at its state's +0x87C
constexpr uint32_t kSysMode = 0x0047E224u;    // SysGetCurMode's word
constexpr uint32_t kFieldMessages = 0x00481BBCu; // MsgGetActiveTable's table in mode 0
constexpr uint32_t kMode1Messages = 0x00484180u; // and in mode 1
constexpr uint32_t kVsMode = 6u;              // the PAL takes mode 0's table there too
constexpr int32_t kButtonMiddle = 26;

struct LayoutWord
{
    uint16_t widget;
    uint32_t text; // which of its texts
    uint32_t message;
    bool system;  // a system message, else the battle's
    bool centred; // centred at kButtonMiddle
};
constexpr LayoutWord kLayoutWords[] = {
    {0xC37B, 0, 0x52, true, true},
    {0xC37C, 0, 0x53, true, true},
    {0xC36E, 1, 5004, false, false},
    {0xC382, 0, 5005, false, false},
};

// How the game uses an overlay string: as sprintf's format into a piece's
// 6-byte slot of the roster (InsectBattleRosterListRefresh, 0x4FB030 and
// 0x4FB058: at most 5 characters printed), "ACE" with no argument and
// "Lv%d" with a level (at most 3 digits, as the US one allows); or appended
// to the victory code's text (strcat).
enum class Use : uint8_t
{
    Format,
    LevelFormat,
    Appended,
};
constexpr uint32_t kSlotCharacters = 5;
constexpr uint32_t kLevelDigits = 3;

struct OverlayWord
{
    uint32_t address;
    uint32_t room; // bytes, its terminator included
    uint32_t message;
    Use use;
};
constexpr OverlayWord kOverlayWords[] = {
    {0x00524C28u, 8, 5000, Use::Format},
    {0x00524C30u, 16, 5001, Use::LevelFormat},
    {0x00524BA8u, 8, 5003, Use::Appended},
};

// --- The Vs. menus (INSMENU.BIN) ------------------------------------------
// Opened from the title's Insectron row. The PAL overlay sizes each panel's
// rows per language from tables in its own data (0x507D80 on). Where its
// English value is the US one, the language's value replaces the US constant
// at the US call:
// - Watch Battle (InsectVsWatchPanelLayout 0x4F11D0, its rows' bar
//   InsectVsMenuBarToRowResize 0x4EE350): 220 wide (PAL 0x507ED0);
// - Password Battle (0x4F1430, bar 0x4EE3D0): 220 (0x507EB0);
// - 2-Player Battle (0x4F1C80, bar 0x4EE450): 220 (0x507E90);
// each with its frame (UiLayoutFrameRectsMaybe, a1), the list's frame
// (InsectVsListFrameFit, a2), the highlight (InsectVsHighlightBarSize, a1)
// and the bar (UiSetSpriteSize of 0x21 or 0x13, a2, 12 narrower); French
// "Charger données Joueur 1", Spanish "Selección de datos de jugador" and
// German "Datenauswahl Spieler 2" ran past their rows;
// - the Registration panels' "register from a password", "save" and "view"
//   (InsectVsRegPwPanelLayout 0x4F06A0, RegSave 0x4EFD90, RegView 0x4EF4C0):
//   176 wide (0x507EF0, 0x507F20, 0x507F50), the bar 12 narrower; "delete"
//   (RegDelete 0x4EF220) is 176 in every language there too (0x507F70).
constexpr int32_t kWatch[4] = {245, 240, 305, 220};
constexpr int32_t kPassword[4] = {260, 240, 305, 220};
constexpr int32_t kTwoPlayer[4] = {265, 225, 270, 220};
constexpr int32_t kRegPassword[4] = {211, 196, 226, 186};
constexpr int32_t kRegSave[4] = {186, 186, 216, 186};
constexpr int32_t kRegView[4] = {186, 176, 176, 176};
constexpr int32_t kPanelRowUs = 220;
constexpr int32_t kRegRowUs = 176;
constexpr int32_t kBarInset = 12;

// The top menu (InsectVsTopPanelLayout 0x4F2C20, its focus 0x4F2680 and its
// rows' bar 0x4EE570) and the Registration menu (0x4F2500, 0x4F1F80, bar
// 0x4EE4D0) are laid out differently in the PAL overlay, in English too: there
// the language's width is set against PAL English's. The top menu's rows are
// 306 wide (PAL 0x507E20: English 311, French 351, German 306, Spanish 358,
// Italian 296) with the mode's players block at their right end (moved after
// the layout, as the PAL code places it at the width less 180), the highlight
// 12 narrower, and "Return to title" 130 (0x507E40, English as US). The
// Registration rows are 250 (0x507E50, English 225) and its "Back" 125
// (0x507E60, English 59). French "Combat mot de passe" and Spanish "Batalla
// con contraseña" ran into the players block; Spanish "Registrarse con datos
// guardados" past its row.
constexpr int32_t kTopWider[4] = {40, -5, 47, -15};
constexpr int32_t kTopRowsUs = 306;
constexpr int32_t kTopBack[4] = {155, 140, 130, 125};
constexpr int32_t kTopBackUs = 130;
constexpr int32_t kRegisterWider[4] = {5, 33, 60, -5};
constexpr int32_t kRegisterRowsUs = 250;
constexpr int32_t kRegisterBackWider[4] = {13, 15, 5, 17};
constexpr int32_t kRegisterBackUs = 125;

constexpr uint32_t kVsUi = 0x004FA180u; // INSMENU's UI

// InsectVsListFrameFit centres the list's label (the picture "Battle Start!!",
// widgets 0x49 and 0x43) in the list's width by the label's sprite width, the
// US layout's 160. The European pictures are the PAL layout's, 230 wide with
// the words at their left, and the PAL code centres them by the words' width
// instead (0x507F90: English 160, Spanish 176, French 188, Italian 174, German
// 158): by the sprite's, Italian "Inizio Scontro!!" lost its first letter
// behind the frame's edge.
constexpr int32_t kLabelWords[4] = {188, 158, 176, 174};

// The top menu's players block ("1P VS CPU", its icons and the arrows of
// Registration) is placed by the layout alone in the US overlay, from x 128 at
// the rows' right end; the PAL code places it at the rows' width less 180
// (InsectVsTopPanelLayout's end). Its pieces: the second widget 0x39 (the
// block's back, 6 sprites; the first 0x39 is the title banner), 0x3B (the
// icons, VS and arrows, 11 sprites) and 0x38 (the players' names, 6 texts),
// each told by its first piece's US x. They move by the rows' growth, once:
// a piece already there is left.
struct BlockPiece
{
    uint16_t id;
    bool texts;
    int count;
    int16_t usX; // its first piece's x in the US layout
};
constexpr BlockPiece kPlayersBlock[] = {
    {0x39u, false, 6, 128},
    {0x3Bu, false, 11, 129},
    {0x38u, true, 6, 181},
};

// The title banner along the top (InsectVsTitleBannerLayout 0x4EA600, the first
// widget 0x39, the layout's group 30240). Its sprites: the bar (0 its back, from
// -4 and 320 wide; 1 its slanted end at 316; 2 the rest, from 352, 180 wide),
// the mode's title (3, a row of the picture InscPW_03a, told by its v) and the
// mode's pieces after it: two icons (4, 5), "VS" (6) and the arrows (7, 8), at
// the x the US code sets per mode for the English words (-1: not shown). The
// PAL code places those pieces after the title by its words' width, from
// tables per mode and language (0x507D80 to 0x507E00: English, Spanish, French,
// Italian, German; the words' right edge and 3 to 5), and ends the bar after
// the last piece. Here the pieces move by the language's width less the US
// English one, and the bar's end moves right as far, when they do: Spanish
// "Batalla con contraseña" ran under its icons, French "Combat mot de passe"
// reached them.
struct BannerRow
{
    int16_t v;         // the title's row in the picture
    int16_t english;   // the US English title's width, in the PAL tables' measure
    int16_t width[4];  // French, German, Spanish, Italian (PAL)
    int16_t pieces[5]; // sprites 4 to 8's x in that mode, in the US code (-1: none)
};
constexpr BannerRow kBannerRows[] = {
    // "Insectron": 166 in PAL English, whose picture is wider; the US one is as
    // wide as the European ones.
    {0x58, 158, {158, 158, 158, 158}, {-1, -1, -1, -1, -1}},
    {0x74, 180, {174, 132, 228, 140}, {232, 278, 259, -1, -1}}, // 2-Player Battle
    {0x90, 194, {212, 198, 234, 116}, {232, 278, 259, -1, -1}}, // Password Battle
    {0xAC, 154, {180, 180, 170, 164}, {232, 278, 259, -1, -1}}, // Watch Battle
    {0xC8, 142, {116, 136, 100, 160}, {226, 288, -1, 253, 253}}, // Registration
    {0xE4, 280, {276, 296, 276, 254}, {-1, -1, -1, -1, -1}},     // its panels
    {0x100, 280, {266, 248, 292, 268}, {-1, -1, -1, -1, -1}},
    {0x11C, 250, {232, 300, 246, 242}, {278, -1, -1, -1, -1}},
    {0x138, 268, {266, 294, 264, 268}, {294, -1, -1, -1, -1}},
};
constexpr uint16_t kBannerWidget = 0x39u;
constexpr int kBannerTitle = 3;
constexpr int kBannerFirstPiece = 4;
constexpr uint32_t kSpriteX = 0x0u;
constexpr uint32_t kSpriteV = 0xAu;
// The bar as the US layout has it.
constexpr int16_t kBarBackWidth = 320; // sprite 0
constexpr int16_t kBarEndX = 316;      // sprite 1
constexpr int16_t kBarRestX = 352;     // sprite 2
constexpr int16_t kBarRestWidth = 180;

// --- The patches, by the function called -------------------------------
// Three of these functions are hooked by text_language.cpp too (UiLayout-
// WindowFrame and UiSetSpriteW for the cage's commands, UiMoveWidgetUniform
// for the name entry): these hooks wrap those, installed after them.
// UiLayoutWindowFrame (0x28B8E0): a1 the frame's width.
constexpr ArgPatch kWindowFramePatches[] = {
    same(From::Main, 0x00396F28u, kA1, 94, kCaught),
    same(From::Main, 0x00396FF8u, kA1, 140, kEmpty),
};
// UiSetSpriteW (0x28FBF0): a2 the width. The notices' highlights; the
// highlight moving to a row of the top menu or of the Registration menu
// (their focus and their rows' bars): the rows' width less 12, or the last
// row's ("Return to title", "Back").
constexpr ArgPatch kSpriteWPatches[] = {
    narrower(From::Main, 0x00396F78u, kA2, 94, kCaught, kNoticeInset),
    narrower(From::Main, 0x00397048u, kA2, 140, kEmpty, kNoticeInset),
    wider(From::Insmenu, 0x004EE5D0u, kA2, kTopRowsUs - kBarInset, kTopWider),
    narrower(From::Insmenu, 0x004EE5ECu, kA2, kTopBackUs, kTopBack, kBarInset),
    wider(From::Insmenu, 0x004F27B4u, kA2, kTopRowsUs - kBarInset, kTopWider),
    narrower(From::Insmenu, 0x004F27D4u, kA2, kTopBackUs, kTopBack, kBarInset),
    wider(From::Insmenu, 0x004EE530u, kA2, kRegisterRowsUs - kBarInset, kRegisterWider),
    wider(From::Insmenu, 0x004EE54Cu, kA2, kRegisterBackUs - kBarInset, kRegisterBackWider),
    wider(From::Insmenu, 0x004F20B4u, kA2, kRegisterRowsUs - kBarInset, kRegisterWider),
    wider(From::Insmenu, 0x004F20D4u, kA2, kRegisterBackUs - kBarInset, kRegisterBackWider),
};
// UiSetWidgetPos (0x290F60): a2 x. The notices' frames and highlights; the
// battle's help box (0x7556 at 200) and team panels (0x753F from -192); the
// 2-Player panel's controller marks (0x45) at the panel's width less 8; the
// help list's scroll bar (0x75FD at 276; its window's x, centred, is in
// widgetPos).
constexpr ArgPatch kWidgetPosPatches[] = {
    leftOf(From::Main, 0x00396BDCu, kA2, 348, 94, kCaught),
    leftOf(From::Main, 0x00396FA4u, kA2, 339 + kNoticeInset, 94, kCaught),
    leftOf(From::Main, 0x003973C8u, kA2, 385, 140, kEmpty),
    leftOf(From::Main, 0x00397074u, kA2, 376 + kNoticeInset, 140, kEmpty),
    wider(From::Insect, 0x00500734u, kA2, 0xC8, kPanelWider),
    {From::Insect,
     0x00500970u,
     kA2,
     -kPanelUs,
     {-kPanelUs - kPanelWider[0], -kPanelUs - kPanelWider[1], -kPanelUs - kPanelWider[2], -kPanelUs - kPanelWider[3]}},
    narrower(From::Insmenu, 0x004F1DF4u, kA2, kPanelRowUs, kTwoPlayer, 8),
    wider(From::Help, 0x0050AE0Cu, kA2, kHelpScrollXUs, kHelpWider),
};
// UiQuadSetByModeRelated (0x3934D0): a2 the width. The commands' highlight;
// the team panel's bar 0x7542; the help list's back and highlight.
constexpr ArgPatch kQuadSetPatches[] = {
    narrower(From::Insect, kCommandHighlightReturn, kA2, kCommandWidthUs, kCommandWidth,
             kCommandWidthUs - kCommandHighlightUs),
    wider(From::Insect, 0x005007B0u, kA2, 0xB8, kPanelWider),
    wider(From::Help, 0x0050ACC4u, kA2, kHelpListUs, kHelpWider),
    wider(From::Help, 0x0050AD1Cu, kA2, kHelpListUs, kHelpWider),
};
// UiNineSliceSpritesLayout (0x3930D0): a2 the width. The help list's window.
constexpr ArgPatch kNineSliceSpritesPatches[] = {
    wider(From::Help, kHelpWindowReturn, kA2, kHelpWindowUs, kHelpWider),
};
// INSECT's InsectMsgCenterX (0x512B10): a2 the width to centre a message in.
// The help list's title.
constexpr ArgPatch kCenterXPatches[] = {
    wider(From::Help, 0x0050AF24u, kA2, kHelpWindowUs, kHelpWider),
};
// MemStackAllocQw (0x14D950): a1 the quadwords. The help's topic titles.
constexpr ArgPatch kStackAllocQwPatches[] = {
    {From::HelpTitles,
     kHelpTitleReturn,
     kA1,
     kHelpTitleQwUs,
     {kHelpTitleQw[0], kHelpTitleQw[1], kHelpTitleQw[2], kHelpTitleQw[3]}},
};
// InsectBattleMenuFrameNineSliceMaybe (0x393AC0): a2 the width. The team
// panel's help box (the panel itself, below, also sets its pieces).
constexpr ArgPatch kNineSlicePatches[] = {
    wider(From::Insect, 0x005007D4u, kA2, kPanelUs, kPanelWider),
};
// UiLayoutFrameRectsMaybe (0x28BBB0): a1 the panel's width.
constexpr ArgPatch kFrameRectsPatches[] = {
    wider(From::Insmenu, 0x004F2C64u, kA1, kTopRowsUs, kTopWider),
    same(From::Insmenu, 0x004F2CA0u, kA1, kTopBackUs, kTopBack),
    wider(From::Insmenu, 0x004F2544u, kA1, kRegisterRowsUs, kRegisterWider),
    wider(From::Insmenu, 0x004F2580u, kA1, kRegisterBackUs, kRegisterBackWider),
    same(From::Insmenu, 0x004F12F4u, kA1, kPanelRowUs, kWatch),
    same(From::Insmenu, 0x004F1564u, kA1, kPanelRowUs, kPassword),
    same(From::Insmenu, 0x004F1D7Cu, kA1, kPanelRowUs, kTwoPlayer),
    same(From::Insmenu, 0x004F0810u, kA1, kRegRowUs, kRegPassword),
    same(From::Insmenu, 0x004EFE94u, kA1, kRegRowUs, kRegSave),
    same(From::Insmenu, 0x004EF504u, kA1, kRegRowUs, kRegView),
};
// UiSetSpriteSize (0x28FC30): a2 the bar's width, the panel's less 12.
constexpr ArgPatch kSpriteSizePatches[] = {
    wider(From::Insmenu, 0x004F2E08u, kA2, kTopRowsUs - kBarInset, kTopWider),
    wider(From::Insmenu, 0x004F263Cu, kA2, kRegisterRowsUs - kBarInset, kRegisterWider),
    narrower(From::Insmenu, 0x004F13ECu, kA2, kPanelRowUs, kWatch, kBarInset),
    narrower(From::Insmenu, 0x004F165Cu, kA2, kPanelRowUs, kPassword, kBarInset),
    narrower(From::Insmenu, 0x004F1F30u, kA2, kPanelRowUs, kTwoPlayer, kBarInset),
    narrower(From::Insmenu, 0x004F09BCu, kA2, kRegRowUs, kRegPassword, kBarInset),
    narrower(From::Insmenu, 0x004F0090u, kA2, kRegRowUs, kRegSave, kBarInset),
    narrower(From::Insmenu, 0x004EF76Cu, kA2, kRegRowUs, kRegView, kBarInset),
};
// InsectVsListFrameFit (0x4EA4E0): a2 the list's width.
constexpr ArgPatch kListFramePatches[] = {
    same(From::Insmenu, 0x004F13A8u, kA2, kPanelRowUs, kWatch),
    same(From::Insmenu, 0x004F1618u, kA2, kPanelRowUs, kPassword),
    same(From::Insmenu, 0x004F1EECu, kA2, kPanelRowUs, kTwoPlayer),
};
// InsectVsHighlightBarSize (0x4EA410): a1 the highlight's width, from the
// panels and from their rows' bars.
constexpr ArgPatch kHighlightPatches[] = {
    same(From::Insmenu, 0x004F13B8u, kA1, kPanelRowUs, kWatch),
    same(From::Insmenu, 0x004EE3A8u, kA1, kPanelRowUs, kWatch),
    same(From::Insmenu, 0x004EE3BCu, kA1, kPanelRowUs, kWatch),
    same(From::Insmenu, 0x004F1628u, kA1, kPanelRowUs, kPassword),
    same(From::Insmenu, 0x004EE428u, kA1, kPanelRowUs, kPassword),
    same(From::Insmenu, 0x004EE43Cu, kA1, kPanelRowUs, kPassword),
    same(From::Insmenu, 0x004F1EFCu, kA1, kPanelRowUs, kTwoPlayer),
    same(From::Insmenu, 0x004EE4A8u, kA1, kPanelRowUs, kTwoPlayer),
    same(From::Insmenu, 0x004EE4BCu, kA1, kPanelRowUs, kTwoPlayer),
};

constexpr uint32_t kWindowFrame = 0x0028B8E0u;  // UiLayoutWindowFrame
constexpr uint32_t kSpriteW = 0x0028FBF0u;      // UiSetSpriteW
constexpr uint32_t kWidgetPos = 0x00290F60u;    // UiSetWidgetPos
constexpr uint32_t kWidgetMove = 0x00290F50u;   // UiMoveWidgetUniform (slides to; a j to UiMoveWidget): a1, a2 x
constexpr uint32_t kTextX = 0x0028F590u;        // UiSetTextX: a1 the widget, a2 x
constexpr uint32_t kFrameLayout = 0x00392250u;  // InsectBattleMenuFrameLayoutMaybe: t2 the width
constexpr uint32_t kQuadSet = 0x003934D0u;      // UiQuadSetByModeRelated
constexpr uint32_t kNineSlice = 0x00393AC0u;    // InsectBattleMenuFrameNineSliceMaybe
constexpr uint32_t kFillSetSize = 0x003938F0u;  // InsectUiWindowFillSetSizeMaybe: f12 the width
constexpr uint32_t kFrameRects = 0x0028BBB0u;   // UiLayoutFrameRectsMaybe
constexpr uint32_t kSpriteSize = 0x0028FC30u;   // UiSetSpriteSize
constexpr uint32_t kListFrameFit = 0x004EA4E0u; // INSMENU's InsectVsListFrameFit
constexpr uint32_t kHighlightBar = 0x004EA410u; // INSMENU's InsectVsHighlightBarSize
constexpr uint32_t kTopLayout = 0x004F2C20u;    // INSMENU's InsectVsTopPanelLayout
constexpr uint32_t kBannerLayout = 0x004EA600u; // INSMENU's InsectVsTitleBannerLayout
constexpr uint32_t kNineSliceSprites = 0x003930D0u; // UiNineSliceSpritesLayout
constexpr uint32_t kFillColumn = 0x003939E0u;       // UiSpriteListFillColumnMaybe: f12 the width
constexpr uint32_t kCenterX = 0x00512B10u;          // INSECT's InsectMsgCenterX
constexpr uint32_t kStackAllocQw = 0x0014D950u;     // MemStackAllocQw: a1 the quadwords
constexpr uint32_t kStatsText = 0x004F1410u;        // INSECT's InsectBattleDrawTextRec: a1 the record
constexpr uint32_t kMsgSetBank = 0x00512B80u;       // INSECT's InsectMsgSetBank: a0 the bank
constexpr uint32_t kMsgGet = 0x00512B70u;           // INSECT's InsectMsgGet(bank, id)
constexpr uint32_t kMsgTableLookup = 0x0028AA30u;   // MsgTableLookup(table, id)
constexpr uint32_t kUiSetTextString = 0x0028F510u;  // UiSetTextString(ui, id, text, which)
constexpr uint32_t kUiSetTextCenteredX = 0x0028F300u; // UiSetTextCenteredX(ui, id, x, which)

PS2Runtime::RecompiledFunction g_originalWindowFrame = nullptr;
PS2Runtime::RecompiledFunction g_originalSpriteW = nullptr;
PS2Runtime::RecompiledFunction g_originalWidgetPos = nullptr;
PS2Runtime::RecompiledFunction g_originalWidgetMove = nullptr;
PS2Runtime::RecompiledFunction g_originalTextX = nullptr;
PS2Runtime::RecompiledFunction g_originalFrameLayout = nullptr;
PS2Runtime::RecompiledFunction g_originalQuadSet = nullptr;
PS2Runtime::RecompiledFunction g_originalNineSlice = nullptr;
PS2Runtime::RecompiledFunction g_originalFillSetSize = nullptr;
PS2Runtime::RecompiledFunction g_originalFrameRects = nullptr;
PS2Runtime::RecompiledFunction g_originalSpriteSize = nullptr;
PS2Runtime::RecompiledFunction g_originalListFrameFit = nullptr;
PS2Runtime::RecompiledFunction g_originalHighlightBar = nullptr;
PS2Runtime::RecompiledFunction g_originalTopLayout = nullptr;
PS2Runtime::RecompiledFunction g_originalBannerLayout = nullptr;
PS2Runtime::RecompiledFunction g_originalNineSliceSprites = nullptr;
PS2Runtime::RecompiledFunction g_originalFillColumn = nullptr;
PS2Runtime::RecompiledFunction g_originalCenterX = nullptr;
PS2Runtime::RecompiledFunction g_originalStackAllocQw = nullptr;
PS2Runtime::RecompiledFunction g_originalStatsText = nullptr;
PS2Runtime::RecompiledFunction g_originalMsgSetBank = nullptr;

void windowFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kWindowFramePatches);
    g_originalWindowFrame(rdram, ctx, runtime);
}

void spriteW(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kSpriteWPatches);
    g_originalSpriteW(rdram, ctx, runtime);
}

void widgetPos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t from = getRegU32(ctx, kRa);
    const bool header = from == kCommandHeaderAtReturn;
    if ((header || from == kCommandFrameAtReturn) && reg(ctx, kS4) == kModeRight)
    {
        const int column = columnFor(From::Insect);
        const uint32_t widget = getRegU32(ctx, kA1) & 0xFFFFu;
        const int32_t width = column >= 0 ? kCommandWidth[column] : kCommandWidthUs;
        const int32_t us = reg(ctx, kS1);
        if (column >= 0 && !header && widget == kCommandFrame)
        {
            setReg(ctx, kA2, std::max(0, us - width));
        }
        else if (column >= 0 && header && widget == kCommandHeader)
        {
            setReg(ctx, kA2, reg(ctx, kA2) - (std::max(width, us) - us));
        }
    }
    else if (from == kHelpWindowAtReturn)
    {
        // Centred: the screen's width less the window's, halved.
        const int column = columnFor(From::Help);
        if (column >= 0 && (getRegU32(ctx, kA1) & 0xFFFFu) == kHelpWindow)
        {
            setReg(ctx, kA2, reg(ctx, kA2) - kHelpWider[column] / 2);
        }
    }
    else
    {
        applyPatches(ctx, kWidgetPosPatches);
    }
    g_originalWidgetPos(rdram, ctx, runtime);
}

void widgetMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t from = getRegU32(ctx, kRa);
    if (from == kSpecialSlideReturn || from == kConfirmSlideReturn)
    {
        const int column = columnFor(From::Insect);
        const uint32_t widget = getRegU32(ctx, kA1) & 0xFFFFu;
        const int32_t x = reg(ctx, kA2);
        if (column >= 0 && from == kSpecialSlideReturn && widget == kSpecialList &&
            reg(ctx, kS0) == kSpecialSlideUs)
        {
            setReg(ctx, kA2, x + kCommandWidth[column] - kCommandWidthUs);
        }
        else if (column >= 0 && from == kConfirmSlideReturn && widget == kPanels)
        {
            setReg(ctx, kA2, x - kPanelWider[column]);
        }
    }
    g_originalWidgetMove(rdram, ctx, runtime);
}

void textX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, kRa) == kTitleXReturn && (getRegU32(ctx, kA1) & 0xFFFFu) == kTitle)
    {
        if (const int column = columnFor(From::Insect); column >= 0)
        {
            setReg(ctx, kA2, reg(ctx, kA2) + kPanelWider[column] / 2);
        }
    }
    g_originalTextX(rdram, ctx, runtime);
}

void frameLayout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, kRa) == kCommandFrameReturn && reg(ctx, kT2) == -1)
    {
        if (const int column = columnFor(From::Insect); column >= 0)
        {
            setReg(ctx, kT2, kCommandWidth[column]);
        }
    }
    g_originalFrameLayout(rdram, ctx, runtime);
}

void quadSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kQuadSetPatches);
    g_originalQuadSet(rdram, ctx, runtime);
}

void afterPanel(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t ui)
{
    const int column = columnFor(From::Insect);
    if (column < 0 || kPanelWider[column] == 0 || ui == 0u)
    {
        return;
    }
    for (uint32_t id : kPieceWidgets)
    {
        setSpriteWidth(rdram, ui, id, 0, kPieceWidthUs, kPieceWidthUs + kPanelWider[column]);
    }
    for (int row = 0; row < kRowCount; ++row)
    {
        setSpriteWidth(rdram, ui, kRows, row, kRowWidthUs, kRowWidthUs + kPanelWider[column]);
    }
}

void nineSlice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t from = getRegU32(ctx, kRa);
    if (from == kCommandHeaderReturn || from == kPanelReturn)
    {
        const int column = columnFor(From::Insect);
        const int32_t width = reg(ctx, kA2);
        if (column >= 0 && from == kCommandHeaderReturn)
        {
            setReg(ctx, kA2, std::max(kCommandWidth[column], width));
        }
        else if (column >= 0 && width == kPanelUs && kPanelWider[column] != 0)
        {
            setReg(ctx, kA2, kPanelUs + kPanelWider[column]);
            const uint32_t ui = readWord(rdram, getRegU32(ctx, kS1) + kTeamUiOffset);
            rg::hooks::callThen(g_originalNineSlice, rdram, ctx, runtime, &afterPanel, ui);
            return;
        }
    }
    else
    {
        applyPatches(ctx, kNineSlicePatches);
    }
    g_originalNineSlice(rdram, ctx, runtime);
}

void fillSetSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, kRa) == kHelpFillReturn && ctx->f[12] == kHelpFillUs)
    {
        if (const int column = columnFor(From::Insect); column >= 0)
        {
            ctx->f[12] = kHelpFillUs + static_cast<float>(kPanelWider[column]);
        }
    }
    g_originalFillSetSize(rdram, ctx, runtime);
}

void nineSliceSprites(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kNineSliceSpritesPatches);
    g_originalNineSliceSprites(rdram, ctx, runtime);
}

void fillColumn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, kRa) == kHelpRowsReturn && ctx->f[12] == kHelpRowsUs)
    {
        if (const int column = columnFor(From::Help); column >= 0)
        {
            ctx->f[12] = kHelpRowsUs + static_cast<float>(kHelpWider[column]);
        }
    }
    g_originalFillColumn(rdram, ctx, runtime);
}

void centerX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kCenterXPatches);
    g_originalCenterX(rdram, ctx, runtime);
}

void stackAllocQw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kStackAllocQwPatches);
    g_originalStackAllocQw(rdram, ctx, runtime);
}

void statsText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int column = columnFor(From::Stats);
    const uint32_t record = getRegU32(ctx, kA1);
    const int16_t y = column >= 0 ? readHalf(rdram, record + kRecordY) : 0;
    if (y == kStatsRowY[0] || y == kStatsRowY[1])
    {
        const int16_t x = readHalf(rdram, record);
        const auto packed = static_cast<uint16_t>(readHalf(rdram, record + kRecordAlign));
        const int align = packed >> kAlignShift;
        for (int k = 0; k < kStatsTexts; ++k)
        {
            if (x == kStatsXUs[k] && align == kStatsAlignUs[k])
            {
                writeHalf(rdram, record, kStatsX[column][k]);
                const unsigned low = packed & ((1u << kAlignShift) - 1u);
                writeHalf(rdram, record + kRecordAlign,
                          static_cast<int16_t>(low | (static_cast<unsigned>(kStatsAlign[column][k]) << kAlignShift)));
                break;
            }
        }
    }
    g_originalStatsText(rdram, ctx, runtime);
}

// The game's function at `entry`, run to its end with `args` in a0 on: its
// v0, or 0 if it could not finish.
uint32_t callGame(uint8_t *rdram, const R5900Context *ctx, PS2Runtime *runtime, uint32_t entry,
                  std::initializer_list<uint32_t> args)
{
    PS2Runtime::RecompiledFunction function = runtime->lookupFunction(entry);
    if (!function)
    {
        return 0u;
    }
    R5900Context local = *ctx;
    int r = kA0;
    for (const uint32_t arg : args)
    {
        setReg(&local, r++, static_cast<int32_t>(arg));
    }
    if (!rg::hooks::runToEnd(function, entry, rdram, local, runtime))
    {
        std::fprintf(stderr, "[rg] an Insectron word stopped at 0x%08X\n", local.pc);
        return 0u;
    }
    return getRegU32(&local, kV0);
}

// The bytes (its terminator included) of the message at `text` if it may
// stand in for `word`'s US string, else 0: it fits the string's room, and as
// a format it has the US one's conversions (none, or one %d) and prints no
// more than the slot takes.
uint32_t standInBytes(const uint8_t *rdram, uint32_t text, const OverlayWord &word)
{
    uint32_t percents = 0;
    bool levels = true; // each % a %d
    for (uint32_t i = 0; i < word.room; ++i)
    {
        const uint8_t c = rdram[(text + i) & 0x1FFFFFFFu];
        if (c == 0u)
        {
            bool fits = true;
            if (word.use == Use::Format)
            {
                fits = percents == 0u && i <= kSlotCharacters;
            }
            else if (word.use == Use::LevelFormat)
            {
                fits = percents == 1u && levels && i - 2u + kLevelDigits <= kSlotCharacters;
            }
            return fits ? i + 1u : 0u;
        }
        if (c == '%')
        {
            ++percents;
            levels = levels && rdram[(text + i + 1u) & 0x1FFFFFFFu] == 'd';
        }
    }
    return 0u;
}

void afterMsgSetBank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t bank)
{
    if (columnFor(From::Words) < 0)
    {
        return;
    }
    const uint32_t ui = readWord(rdram, bank - kBattleMessages + kBattleUi);
    const uint32_t mode = readWord(rdram, kSysMode);
    const uint32_t system = readWord(rdram, mode == 1u ? kMode1Messages : kFieldMessages);
    const bool systemOn = mode == 0u || mode == 1u || mode == kVsMode;
    for (const LayoutWord &word : kLayoutWords)
    {
        uint32_t text = 0u;
        if (!word.system)
        {
            text = callGame(rdram, ctx, runtime, kMsgGet, {bank, word.message});
        }
        else if (systemOn && system != 0u)
        {
            text = callGame(rdram, ctx, runtime, kMsgTableLookup, {system, word.message});
        }
        if (ui == 0u || text == 0u)
        {
            continue;
        }
        callGame(rdram, ctx, runtime, kUiSetTextString, {ui, word.widget, text, word.text});
        if (word.centred)
        {
            callGame(rdram, ctx, runtime, kUiSetTextCenteredX,
                     {ui, word.widget, static_cast<uint32_t>(kButtonMiddle), word.text});
        }
    }
    for (const OverlayWord &word : kOverlayWords)
    {
        const uint32_t text = callGame(rdram, ctx, runtime, kMsgGet, {bank, word.message});
        const uint32_t bytes = text ? standInBytes(rdram, text, word) : 0u;
        if (bytes != 0u)
        {
            std::memmove(rdram + (word.address & 0x1FFFFFFFu), rdram + (text & 0x1FFFFFFFu), bytes);
        }
    }
}

void msgSetBank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, kRa) != kMsgSetBankReturn || columnFor(From::Words) < 0)
    {
        g_originalMsgSetBank(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalMsgSetBank, rdram, ctx, runtime, &afterMsgSetBank, getRegU32(ctx, kA0));
}

void frameRects(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kFrameRectsPatches);
    g_originalFrameRects(rdram, ctx, runtime);
}

void spriteSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kSpriteSizePatches);
    g_originalSpriteSize(rdram, ctx, runtime);
}

void afterListFrameFit(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t labelAndWidth)
{
    const int column = columnFor(From::Insmenu);
    const uint32_t label = column >= 0 ? findWidget(rdram, kVsUi, static_cast<uint16_t>(labelAndWidth >> 16)) : 0u;
    const uint32_t sprites = label ? readWord(rdram, label + kWidgetSprites) : 0u;
    if (sprites != 0u)
    {
        const int32_t width = static_cast<int16_t>(labelAndWidth & 0xFFFFu);
        writeHalf(rdram, sprites, static_cast<int16_t>(width / 2 - kLabelWords[column] / 2));
    }
}

void listFrameFit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t from = getRegU32(ctx, kRa);
    const bool known = std::any_of(std::begin(kListFramePatches), std::end(kListFramePatches),
                                   [from](const ArgPatch &patch) { return patch.ret == from; });
    if (known && columnFor(From::Insmenu) >= 0)
    {
        applyPatches(ctx, kListFramePatches);
        const uint32_t labelAndWidth = (getRegU32(ctx, kA1) << 16) | (getRegU32(ctx, kA2) & 0xFFFFu);
        rg::hooks::callThen(g_originalListFrameFit, rdram, ctx, runtime, &afterListFrameFit, labelAndWidth);
        return;
    }
    g_originalListFrameFit(rdram, ctx, runtime);
}

void highlightBar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    applyPatches(ctx, kHighlightPatches);
    g_originalHighlightBar(rdram, ctx, runtime);
}

void moveBlockPiece(uint8_t *rdram, uint32_t list, const BlockPiece &piece, int32_t by)
{
    if (readHalf(rdram, list) != piece.usX)
    {
        return; // moved already, or not the layout this was measured on
    }
    const uint32_t stride = piece.texts ? kTextBytes : kSpriteBytes;
    for (int i = 0; i < piece.count; ++i)
    {
        const uint32_t at = list + static_cast<uint32_t>(i) * stride;
        writeHalf(rdram, at, static_cast<int16_t>(readHalf(rdram, at) + by));
    }
}

void afterTopLayout(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    const int column = columnFor(From::Insmenu);
    if (column < 0 || kTopWider[column] == 0)
    {
        return;
    }
    forEachWidget(rdram, kVsUi, [&](uint32_t widget, uint16_t id) {
        for (const BlockPiece &piece : kPlayersBlock)
        {
            const uint32_t list =
                id == piece.id ? readWord(rdram, widget + (piece.texts ? kWidgetTexts : kWidgetSprites)) : 0u;
            if (list != 0u)
            {
                moveBlockPiece(rdram, list, piece, kTopWider[column]);
            }
        }
        return false;
    });
}

void topLayout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_originalTopLayout, rdram, ctx, runtime, &afterTopLayout, 0u);
}

// After the US code has set the mode's title and pieces: the pieces it set
// (those at their US x) moved, and the bar ended `by` further right. The bar is
// left alone unless it is the US layout's, moved right by a step of ours.
void afterBannerLayout(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    const int column = columnFor(From::Banner);
    const uint32_t banner = column >= 0 ? findWidget(rdram, kVsUi, kBannerWidget) : 0u;
    const uint32_t sprites = banner ? readWord(rdram, banner + kWidgetSprites) : 0u;
    if (sprites == 0u)
    {
        return;
    }
    const auto at = [sprites](int sprite, uint32_t field) {
        return sprites + static_cast<uint32_t>(sprite) * kSpriteBytes + field;
    };
    const int16_t v = readHalf(rdram, at(kBannerTitle, kSpriteV));
    const BannerRow *row = std::find_if(std::begin(kBannerRows), std::end(kBannerRows),
                                        [v](const BannerRow &r) { return r.v == v; });
    if (row == std::end(kBannerRows))
    {
        return;
    }
    const int32_t delta = row->width[column] - row->english;
    for (int i = 0; i < static_cast<int>(std::size(row->pieces)); ++i)
    {
        const uint32_t x = at(kBannerFirstPiece + i, kSpriteX);
        if (row->pieces[i] >= 0 && readHalf(rdram, x) == row->pieces[i])
        {
            writeHalf(rdram, x, static_cast<int16_t>(row->pieces[i] + delta));
        }
    }
    const int32_t was = readHalf(rdram, at(1, kSpriteX)) - kBarEndX;
    const bool usBar = was >= 0 && was < kBarRestWidth &&
                       readHalf(rdram, at(0, kSpriteWidth)) == kBarBackWidth + was &&
                       readHalf(rdram, at(2, kSpriteX)) == kBarRestX + was &&
                       readHalf(rdram, at(2, kSpriteWidth)) == kBarRestWidth - was;
    if (usBar)
    {
        const int32_t by = std::clamp(delta, 0, kBarRestWidth - 1);
        writeHalf(rdram, at(0, kSpriteWidth), static_cast<int16_t>(kBarBackWidth + by));
        writeHalf(rdram, at(1, kSpriteX), static_cast<int16_t>(kBarEndX + by));
        writeHalf(rdram, at(2, kSpriteX), static_cast<int16_t>(kBarRestX + by));
        writeHalf(rdram, at(2, kSpriteWidth), static_cast<int16_t>(kBarRestWidth - by));
    }
}

void bannerLayout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (columnFor(From::Banner) < 0)
    {
        g_originalBannerLayout(rdram, ctx, runtime); // English: the US code alone
        return;
    }
    rg::hooks::callThen(g_originalBannerLayout, rdram, ctx, runtime, &afterBannerLayout, 0u);
}

struct Hook
{
    const char *overlay; // null: the main executable's
    uint32_t address;
    PS2Runtime::RecompiledFunction replacement;
    PS2Runtime::RecompiledFunction &original;
};

bool hook(PS2Runtime &runtime, const Hook &h, PS2Runtime::RecompiledFunction replacement,
          PS2Runtime::RecompiledFunction &original)
{
    return h.overlay ? rg::overlay::hookFunction(h.overlay, h.address, replacement, original)
                     : rg::timing::hookFunction(runtime, h.address, replacement, original);
}

// Hooks that only work together, each screen's: all in, or none (those
// already in are put back, in reverse, so that each function gets the entry
// it had, a hook of text_language.cpp's for three of them).
template <size_t N>
bool hookAll(PS2Runtime &runtime, const Hook (&hooks)[N])
{
    for (const Hook *h = std::begin(hooks); h != std::end(hooks); ++h)
    {
        if (!hook(runtime, *h, h->replacement, h->original))
        {
            while (h != std::begin(hooks))
            {
                --h;
                PS2Runtime::RecompiledFunction ours = nullptr;
                hook(runtime, *h, h->original, ours);
            }
            return false;
        }
    }
    return true;
}

} // namespace

void installInsectronLayouts(PS2Runtime &runtime)
{
    static bool installed = false;
    if (installed)
    {
        return; // a second time would hook its own hooks
    }
    installed = true;
    // Shared by the screens that change widths: without them, none of those.
    const Hook shared[] = {
        {nullptr, kSpriteW, &spriteW, g_originalSpriteW},
        {nullptr, kWidgetPos, &widgetPos, g_originalWidgetPos},
    };
    const bool widthsOn = hookAll(runtime, shared);
    if (!widthsOn)
    {
        std::fprintf(stderr, "[rg] the Insectron screens keep the US widths\n");
    }
    const Hook notices[] = {
        {nullptr, kWindowFrame, &windowFrame, g_originalWindowFrame},
    };
    const Hook battle[] = {
        {nullptr, kWidgetMove, &widgetMove, g_originalWidgetMove},
        {nullptr, kTextX, &textX, g_originalTextX},
        {nullptr, kFrameLayout, &frameLayout, g_originalFrameLayout},
        {nullptr, kQuadSet, &quadSet, g_originalQuadSet},
        {nullptr, kNineSlice, &nineSlice, g_originalNineSlice},
        {nullptr, kFillSetSize, &fillSetSize, g_originalFillSetSize},
    };
    // The help list's back and highlight go through the battle's
    // UiQuadSetByModeRelated hook: its own hooks only with those.
    const Hook help[] = {
        {nullptr, kNineSliceSprites, &nineSliceSprites, g_originalNineSliceSprites},
        {nullptr, kFillColumn, &fillColumn, g_originalFillColumn},
        {"insect", kCenterX, &centerX, g_originalCenterX},
    };
    const Hook vs[] = {
        {nullptr, kFrameRects, &frameRects, g_originalFrameRects},
        {nullptr, kSpriteSize, &spriteSize, g_originalSpriteSize},
        {"insmenu", kListFrameFit, &listFrameFit, g_originalListFrameFit},
        {"insmenu", kHighlightBar, &highlightBar, g_originalHighlightBar},
        {"insmenu", kTopLayout, &topLayout, g_originalTopLayout},
    };
    const Hook banner[] = {
        {"insmenu", kBannerLayout, &bannerLayout, g_originalBannerLayout},
    };
    const Hook helpTitles[] = {
        {nullptr, kStackAllocQw, &stackAllocQw, g_originalStackAllocQw},
    };
    const Hook stats[] = {
        {"insect", kStatsText, &statsText, g_originalStatsText},
    };
    const Hook words[] = {
        {"insect", kMsgSetBank, &msgSetBank, g_originalMsgSetBank},
    };
    g_screenOn[static_cast<int>(From::Main)] = widthsOn && hookAll(runtime, notices);
    g_screenOn[static_cast<int>(From::Insect)] = widthsOn && hookAll(runtime, battle);
    g_screenOn[static_cast<int>(From::Help)] = g_screenOn[static_cast<int>(From::Insect)] && hookAll(runtime, help);
    g_screenOn[static_cast<int>(From::Insmenu)] = widthsOn && hookAll(runtime, vs);
    // These use none of the shared hooks.
    g_screenOn[static_cast<int>(From::Banner)] = hookAll(runtime, banner);
    g_screenOn[static_cast<int>(From::HelpTitles)] = hookAll(runtime, helpTitles);
    g_screenOn[static_cast<int>(From::Stats)] = hookAll(runtime, stats);
    g_screenOn[static_cast<int>(From::Words)] = hookAll(runtime, words);
    const struct
    {
        From from;
        bool widths; // a width screen: told above when the shared hooks failed
        const char *kept;
    } screens[] = {
        {From::Main, true, "the Insectron capture notices keep the US widths"},
        {From::Insect, true, "the Insectron battle's menus keep the US widths"},
        {From::Help, true, "the Insectron battle's help list keeps the US width"},
        {From::Insmenu, true, "the Insectron Vs. menus keep the US widths"},
        {From::Banner, false, "the Insectron Vs. menus' title banner keeps the US layout"},
        {From::HelpTitles, false, "the Insectron battle's help topics keep the US room for their titles"},
        {From::Stats, false, "the Insectron Vs. battles' statistics keep the US places"},
        {From::Words, false, "the Insectron battle keeps its English Yes, No and labels"},
    };
    for (const auto &screen : screens)
    {
        if (!g_screenOn[static_cast<int>(screen.from)] && (widthsOn || !screen.widths))
        {
            std::fprintf(stderr, "[rg] %s\n", screen.kept);
        }
    }
}

} // namespace rg::textlanguage
