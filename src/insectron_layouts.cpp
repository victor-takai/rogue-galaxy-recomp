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
#include <iterator>

#include "overlay/overlay_manager.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::textlanguage
{
namespace
{
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
// be resident: other code stands at those addresses otherwise).
enum class From : uint8_t
{
    Main,
    Insect,
    Insmenu,
};

// Each screen's hooks went in (installInsectronLayouts), by From.
bool g_screenOn[3] = {false, false, false};

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
    const char *name = from == From::Insect ? "insect" : "insmenu";
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
// 2-Player panel's controller marks (0x45) at the panel's width less 8.
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
};
// UiQuadSetByModeRelated (0x3934D0): a2 the width. The commands' highlight;
// the team panel's bar 0x7542.
constexpr ArgPatch kQuadSetPatches[] = {
    narrower(From::Insect, kCommandHighlightReturn, kA2, kCommandWidthUs, kCommandWidth,
             kCommandWidthUs - kCommandHighlightUs),
    wider(From::Insect, 0x005007B0u, kA2, 0xB8, kPanelWider),
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
    // Shared by the screens: without them, none.
    const Hook shared[] = {
        {nullptr, kSpriteW, &spriteW, g_originalSpriteW},
        {nullptr, kWidgetPos, &widgetPos, g_originalWidgetPos},
    };
    if (!hookAll(runtime, shared))
    {
        std::fprintf(stderr, "[rg] the Insectron screens keep the US widths\n");
        return;
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
    const Hook vs[] = {
        {nullptr, kFrameRects, &frameRects, g_originalFrameRects},
        {nullptr, kSpriteSize, &spriteSize, g_originalSpriteSize},
        {"insmenu", kListFrameFit, &listFrameFit, g_originalListFrameFit},
        {"insmenu", kHighlightBar, &highlightBar, g_originalHighlightBar},
        {"insmenu", kTopLayout, &topLayout, g_originalTopLayout},
    };
    g_screenOn[static_cast<int>(From::Main)] = hookAll(runtime, notices);
    g_screenOn[static_cast<int>(From::Insect)] = hookAll(runtime, battle);
    g_screenOn[static_cast<int>(From::Insmenu)] = hookAll(runtime, vs);
    if (!g_screenOn[static_cast<int>(From::Main)])
    {
        std::fprintf(stderr, "[rg] the Insectron capture notices keep the US widths\n");
    }
    if (!g_screenOn[static_cast<int>(From::Insect)])
    {
        std::fprintf(stderr, "[rg] the Insectron battle's menus keep the US widths\n");
    }
    if (!g_screenOn[static_cast<int>(From::Insmenu)])
    {
        std::fprintf(stderr, "[rg] the Insectron Vs. menus keep the US widths\n");
    }
}

} // namespace rg::textlanguage
