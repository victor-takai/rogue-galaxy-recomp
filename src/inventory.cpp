#include "inventory.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <smmintrin.h>

#include "overlay/overlay_manager.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::inventory
{
namespace
{

constexpr uint32_t kItemDefGetStackLimit = 0x0040E070u;
constexpr uint32_t kInvAddItemCore = 0x003C6540u;
constexpr uint32_t kInvAddItemRepeat = 0x003C47B0u;
// The adds that fall back to the overflow when the bag has no room, by
// their return addresses: an item found. InvAddItemRepeat's (chests, event
// commands and rewards, Insectron prizes), ScriptFnInvAddItem's and
// BattleCalcResults' three (the spoils).
constexpr uint32_t kFoundItemAdds[] = {0x003C480Cu, 0x003D9B18u, 0x003A2308u, 0x003A248Cu, 0x003A2A7Cu};
constexpr uint32_t kRepeatAdd = 0x003C480Cu;
// PartyCharaEquipDefaultsMaybe, which gives a character its equipment
// through InvAddItemRepeat and equips it: not an item found, and equipment
// is kept in the bag.
constexpr uint32_t kEquipDefaultsStart = 0x003C1250u;
constexpr uint32_t kEquipDefaultsEnd = 0x003C1424u;
// The stack limit of an item without its own (99).
constexpr uint32_t kDefaultStackLimit = 0x00465B60u;
// The storage's first and last slots, in the range table.
constexpr uint32_t kStorageFirst = 0x00465A38u;
constexpr uint32_t kStorageLast = 0x00465A40u;
constexpr int32_t kKindBag = 0;
constexpr int32_t kKindBagThenStorage = 10;
// An item object's slot.
constexpr uint32_t kItemSlot = 0x6u;

// The seventh page. The table's words that move: the bag's pages and last
// slot, every first and last slot after the bag's, and the last of kind 10
// (the bag then the storage).
constexpr uint32_t kBagPages = 0x004659E8u;
constexpr uint32_t kBagLast = 0x00465A10u;
constexpr uint32_t kSlotsAfterBag[] = {
    0x00465A38u, 0x00465A40u, // the storage
    0x00465A60u, 0x00465A68u, // key items
    0x00465A78u, 0x00465A80u, // key pieces
    0x00465A90u, 0x00465A98u, // Drigellums
    0x00465AA8u, 0x00465AB0u, // Factory parts
    0x00465AD0u, 0x00465AD8u, // bounty proofs; the last is any kind's too
    0x00465AE8u, 0x00465AF0u, // Insectors
    0x00465B00u, 0x00465B08u, // a shop's scratch
    0x00465B30u, 0x00465B38u, // the overflow
    0x00465B48u,
};
// The pool's size.
constexpr uint32_t kPoolSize = 0x00465B50u;
constexpr int32_t kPageSlots = 40;
// The game's numbers, which saves keep.
constexpr int32_t kGameBagEnd = 240;
constexpr int32_t kGamePoolSize = 726;
constexpr int32_t kBigPoolSize = kGamePoolSize + kPageSlots;

// The party's save chunk (PartySerialize 0x3BF820, PartyDeserialize
// 0x3BF4F0): chunks of a 12-byte header (kind, id, size) and their contents.
// Kind 10 holds the items, a kind 11 for each, the object from +4 on: its
// slot at +2.
constexpr uint32_t kPartySerialize = 0x003BF820u;
constexpr uint32_t kPartyDeserialize = 0x003BF4F0u;
constexpr uint32_t kChunkHeader = 12u;
constexpr uint32_t kChunkParty = 8u;
constexpr uint32_t kChunkItems = 10u;
constexpr uint32_t kChunkItem = 11u;
constexpr uint32_t kSavedSlot = 2u;

// PartyInit, which makes the pool (party+0x168, its size at +0x14).
constexpr uint32_t kPartyInit = 0x003CB6B0u;
constexpr uint32_t kPartyPool = 0x168u;
constexpr uint32_t kPoolCapacity = 0x14u;

// The item menu keeps where it was in D_004824A8: a bag page (0..5), or 6, 7
// or 8 for the sections of the tab after the bag's (key items and the
// rest), and the party remembers it (PartyMenuCursorSet 0x3C0710, index 2).
// MenuItemOpenState and MenuItemSectionEnter take 6 and up for those
// sections, the seventh page's 6 too. The menu's tabs, its page among them
// at +0x11, follow the table: the sections' tab is the seventh page's.
constexpr uint32_t kMenuSection = 0x004824A8u;
constexpr uint32_t kMenuItemGrid = 0x00482374u;
constexpr uint32_t kGridTab = 0x11u;
constexpr uint32_t kPartyMenuCursorSet = 0x003C0710u;
constexpr uint32_t kMenuItemSectionEnter = 0x002B3400u;
constexpr uint32_t kMenuItemSelectTab = 0x002B3550u;
constexpr int32_t kCursorSection = 2;
constexpr int32_t kFirstSection = 6;
constexpr int32_t kLastSection = 8;
// What the party remembers for the seventh page.
constexpr int32_t kSeventhPageRemembered = 9;
struct Range
{
    uint32_t start;
    uint32_t end;
};
constexpr Range kMenuItemGridState{0x002B1E10u, 0x002B2864u};
constexpr Range kMenuItemCommandSelectState{0x002B04E0u, 0x002B0990u};
constexpr Range kMenuItemOpenState{0x002B2870u, 0x002B30BCu};

// The weapons a character can equip, listed by MenuEquipCandidatesFill
// (list[240]) and, in battle, normal_BtlChgPageWeaponListBuild (f7C[240],
// then the held one): ItemIsEquippableBy for each item of the bag, after an
// IterInit; nothing stops them past 240.
constexpr uint32_t kItemIsEquippableBy = 0x003D4750u;
constexpr uint32_t kIterInit = 0x00370170u;
constexpr Range kMenuEquipCandidatesFill{0x0029E290u, 0x0029E4D8u};
constexpr Range kBattleWeaponListBuild{0x00504B50u, 0x00504D4Cu};
constexpr unsigned kMenuEquipCandidates = 240u;
constexpr unsigned kBattleWeapons = 239u;

// The tab rows over the bag's grid (MenuDrawItemTabs, a shop's sell tabs,
// the Think menu's) draw two sprites a tab with UiSetSpritePosSizeUv, for
// the pages and the tab after them, into the widget's list (+0x20, 0x20 B a
// sprite, ended by one whose x is -1) with nothing to stop them past its
// end. The layouts have room for the game's six pages. MenuDrawItemTabs
// writes 16 for eight tabs and its own end after them; the other two write
// two a page, 14, and leave the end to the list. A shorter list is copied
// into one of ours first, just that long and ended.
constexpr uint32_t kUiSetSpritePosSizeUv = 0x0028F710u;
constexpr Range kMenuDrawItemTabs{0x002A4EE0u, 0x002A53F0u};
constexpr Range kShopSellTabs{0x0050E980u, 0x0050EC78u};
constexpr Range kThinkMenuTabs{0x00520500u, 0x005207F4u};
constexpr uint32_t kWidgetSprites = 0x20u;
constexpr uint32_t kWidgetNext = 0x8u;
constexpr uint32_t kSpriteSize = 0x20u;
constexpr uint32_t kItemTabSprites = 16u;
constexpr uint32_t kSellTabSprites = 14u;
constexpr uint32_t kSpritesMax = 64u;

PS2Runtime::RecompiledFunction g_itemDefGetStackLimit = nullptr;
PS2Runtime::RecompiledFunction g_invAddItemCore = nullptr;
PS2Runtime::RecompiledFunction g_invAddItemRepeat = nullptr;
PS2Runtime::RecompiledFunction g_partySerialize = nullptr;
PS2Runtime::RecompiledFunction g_partyDeserialize = nullptr;
PS2Runtime::RecompiledFunction g_partyInit = nullptr;
PS2Runtime::RecompiledFunction g_partyMenuCursorSet = nullptr;
PS2Runtime::RecompiledFunction g_menuItemSectionEnter = nullptr;
PS2Runtime::RecompiledFunction g_menuItemSelectTab = nullptr;
PS2Runtime::RecompiledFunction g_itemIsEquippableBy = nullptr;
PS2Runtime::RecompiledFunction g_iterInit = nullptr;
std::atomic<bool> g_biggerBagPreferred{false};
// Fixed by install().
bool g_biggerBag = false;
// The slots PartyDeserialize's buffer had, put back once it returns.
std::vector<std::pair<uint32_t, int16_t>> g_loadedSlots;
// The equippable weapons the list being built has.
unsigned g_equippable = 0u;
PS2Runtime::RecompiledFunction g_uiSetSpritePosSizeUv = nullptr;
// The layouts' lists we copied, by address, and our copy of each: a menu
// opened again reloads its layout, often where it was.
std::vector<std::pair<uint32_t, uint32_t>> g_tabLists;
// The last InvAddItemRepeat came from PartyCharaEquipDefaultsMaybe (the
// game's thread).
bool g_equipping = false;
std::atomic<bool> g_stackLimit99{false};
std::atomic<bool> g_fullBagToStorage{false};
std::atomic<unsigned> g_sentToStorage{0u};

int32_t readS32(const uint8_t *rdram, uint32_t address)
{
    int32_t value = 0;
    std::memcpy(&value, rdram + address, sizeof(value));
    return value;
}

bool envOn(const char *name)
{
    const char *value = std::getenv(name);
    return value && *value && *value != '0';
}

// An argument register's low word, its upper half kept.
void setRegS32(R5900Context *ctx, int reg, int32_t value)
{
    ctx->r[reg] = _mm_set_epi64x(_mm_extract_epi64(ctx->r[reg], 1), value);
}

void itemDefGetStackLimit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_stackLimit99.load(std::memory_order_relaxed))
    {
        g_itemDefGetStackLimit(rdram, ctx, runtime);
        return;
    }
    setReturnS32(ctx, readS32(rdram, kDefaultStackLimit));
    // A host function stands in for the guest's `jr ra` too (see
    // libc_rand.cpp).
    ctx->pc = getRegU32(ctx, 31);
}

// The object the add returned, if it is in the storage: an item the bag had
// no room for.
void countStorage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    const uint32_t item = getRegU32(ctx, 2) & 0x1FFFFFFFu;
    if (item == 0u || item + kItemSlot + 2u > PS2_RAM_SIZE)
    {
        return;
    }
    int16_t slot = 0;
    std::memcpy(&slot, rdram + item + kItemSlot, sizeof(slot));
    if (slot >= readS32(rdram, kStorageFirst) && slot <= readS32(rdram, kStorageLast))
    {
        g_sentToStorage.fetch_add(1u, std::memory_order_relaxed);
    }
}

void invAddItemRepeat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_equipping = returnPc >= kEquipDefaultsStart && returnPc < kEquipDefaultsEnd;
    g_invAddItemRepeat(rdram, ctx, runtime);
}

// (inventory, item id, kind, count, slot, check only, remainder out): an item
// found, added to the bag for real, goes to the bag then the storage. The bag
// comes first either way, so only what it has no room for changes, and it
// reaches the overflow only when the storage is full too. Other adds (a
// shop's, the Factory's, equipment given) keep the bag alone.
bool toStorage(R5900Context *ctx)
{
    if (!g_fullBagToStorage.load(std::memory_order_relaxed) ||
        static_cast<int32_t>(getRegU32(ctx, 6)) != kKindBag || (getRegU32(ctx, 9) & 0xFFu) != 0u)
    {
        return false;
    }
    const uint32_t returnPc = getRegU32(ctx, 31);
    if (returnPc == kRepeatAdd && g_equipping)
    {
        return false;
    }
    return std::find(std::begin(kFoundItemAdds), std::end(kFoundItemAdds), returnPc) != std::end(kFoundItemAdds);
}

void invAddItemCore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!toStorage(ctx))
    {
        g_invAddItemCore(rdram, ctx, runtime);
        return;
    }
    setRegS32(ctx, 6, kKindBagThenStorage);
    rg::hooks::callThen(g_invAddItemCore, rdram, ctx, runtime, &countStorage, 0u);
}

uint32_t readU32(const uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, rdram + address, sizeof(value));
    return value;
}

int16_t readS16(const uint8_t *rdram, uint32_t address)
{
    int16_t value = 0;
    std::memcpy(&value, rdram + address, sizeof(value));
    return value;
}

void writeS16(uint8_t *rdram, uint32_t address, int16_t value)
{
    std::memcpy(rdram + address, &value, sizeof(value));
}

void writeS32(uint8_t *rdram, uint32_t address, int32_t value)
{
    std::memcpy(rdram + address, &value, sizeof(value));
}

bool inside(const Range &range, uint32_t address)
{
    return address >= range.start && address < range.end;
}

// The seventh page goes in a save as the slots past the game's pool, and what
// follows the bag back where the game has it.
int16_t savedSlot(int16_t slot)
{
    if (slot >= kGameBagEnd && slot < kGameBagEnd + kPageSlots)
        return static_cast<int16_t>(slot - kGameBagEnd + kGamePoolSize);
    if (slot >= kGameBagEnd + kPageSlots && slot < kBigPoolSize)
        return static_cast<int16_t>(slot - kPageSlots);
    return slot;
}

int16_t loadedSlot(int16_t slot)
{
    if (slot >= kGameBagEnd && slot < kGamePoolSize)
        return static_cast<int16_t>(slot + kPageSlots);
    if (slot >= kGamePoolSize && slot < kBigPoolSize)
        return static_cast<int16_t>(slot - kGamePoolSize + kGameBagEnd);
    return slot;
}

// Calls `visit` with the address of each item's slot in a party chunk's
// contents.
template <class Visit> void forEachSavedSlot(uint32_t at, uint32_t size, const uint8_t *rdram, Visit &&visit)
{
    const uint32_t end = std::min<uint32_t>(at + size, PS2_RAM_SIZE);
    while (at + kChunkHeader <= end)
    {
        const uint32_t kind = readU32(rdram, at);
        const uint32_t length = readU32(rdram, at + 8u);
        const uint32_t body = at + kChunkHeader;
        if (length > end - body)
            return;
        if (kind == kChunkItems)
        {
            for (uint32_t item = body; item + kChunkHeader <= body + length;)
            {
                const uint32_t itemLength = readU32(rdram, item + 8u);
                const uint32_t contents = item + kChunkHeader;
                if (itemLength > body + length - contents)
                    break;
                if (readU32(rdram, item) == kChunkItem && itemLength >= kSavedSlot + 2u)
                    visit(contents + kSavedSlot);
                item = contents + itemLength;
            }
        }
        at = body + length;
    }
}

void putBackLoadedSlots(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    for (const auto &[address, slot] : g_loadedSlots)
        writeS16(rdram, address, slot);
    g_loadedSlots.clear();
}

// (party, contents, size): the save's numbers to this run's, in the buffer,
// before the items are made: the same call looks the insect cages up by
// slot. Put back once it returns.
void partyDeserialize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_biggerBag)
    {
        g_partyDeserialize(rdram, ctx, runtime);
        return;
    }
    const uint32_t at = getRegU32(ctx, 5) & 0x1FFFFFFFu;
    const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
    g_loadedSlots.clear();
    if (at != 0u && size > 0)
    {
        forEachSavedSlot(at, static_cast<uint32_t>(size), rdram, [rdram](uint32_t address) {
            const int16_t slot = readS16(rdram, address);
            const int16_t loaded = loadedSlot(slot);
            if (loaded != slot)
            {
                g_loadedSlots.emplace_back(address, slot);
                writeS16(rdram, address, loaded);
            }
        });
    }
    rg::hooks::callThen(g_partyDeserialize, rdram, ctx, runtime, &putBackLoadedSlots, 0u);
}

// What PartySerialize wrote at `at`, a party chunk, to the save's numbers.
// The save's checksum is made after.
void toSavedSlots(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t at)
{
    const int32_t written = static_cast<int32_t>(getRegU32(ctx, 2));
    if (at == 0u || written < static_cast<int32_t>(kChunkHeader) || readU32(rdram, at) != kChunkParty)
        return;
    const uint32_t size = std::min(readU32(rdram, at + 8u), static_cast<uint32_t>(written) - kChunkHeader);
    forEachSavedSlot(at + kChunkHeader, size, rdram, [rdram](uint32_t address) {
        writeS16(rdram, address, savedSlot(readS16(rdram, address)));
    });
}

// (party, buffer, room)
void partySerialize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_biggerBag)
    {
        g_partySerialize(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_partySerialize, rdram, ctx, runtime, &toSavedSlots, getRegU32(ctx, 5) & 0x1FFFFFFFu);
}

void checkPool(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t party)
{
    static bool told = false;
    const uint32_t pool = party ? readU32(rdram, party + kPartyPool) & 0x1FFFFFFFu : 0u;
    const int32_t capacity = pool ? readS32(rdram, pool + kPoolCapacity) : 0;
    if (capacity != kBigPoolSize && !told)
    {
        told = true;
        std::fprintf(stderr, "[rg] bigger bag: the item pool has room for %d objects, not %d\n", capacity,
                     kBigPoolSize);
    }
}

// The game's heap might not have room for the bigger pool: PoolInit then
// makes it smaller without a word.
void partyInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_biggerBag)
    {
        g_partyInit(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_partyInit, rdram, ctx, runtime, &checkPool, getRegU32(ctx, 4) & 0x1FFFFFFFu);
}

// (party, index, value): the seventh page remembered as 9, apart from the
// first section; the overflow's tab (in the menu the bag's overflow opens),
// which the game would take for a section, as the first page.
void partyMenuCursorSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    const int32_t value = static_cast<int32_t>(getRegU32(ctx, 6));
    if (g_biggerBag && static_cast<int32_t>(getRegU32(ctx, 5)) == kCursorSection && value >= kFirstSection &&
        (inside(kMenuItemGridState, returnPc) || inside(kMenuItemCommandSelectState, returnPc)))
    {
        setRegS32(ctx, 6, value == kFirstSection ? kSeventhPageRemembered : 0);
    }
    g_partyMenuCursorSet(rdram, ctx, runtime);
}

// The seventh page, remembered or just left (the sections' tab is the
// eighth), entered as a page of the bag: any value under 6 is.
void menuItemSectionEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int16_t section = readS16(rdram, kMenuSection);
    const uint32_t grid = readU32(rdram, kMenuItemGrid) & 0x1FFFFFFFu;
    const int8_t tab = grid ? static_cast<int8_t>(rdram[grid + kGridTab]) : -1;
    if (g_biggerBag && (section == kSeventhPageRemembered || (section == kFirstSection && tab == kFirstSection)))
        writeS16(rdram, kMenuSection, 0);
    g_menuItemSectionEnter(rdram, ctx, runtime);
}

// (tab, mode, sound): MenuItemOpenState opens a remembered section on tab 6,
// the seventh page now; the sections' tab is the eighth.
void menuItemSelectTab(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int16_t section = readS16(rdram, kMenuSection);
    const uint32_t grid = readU32(rdram, kMenuItemGrid) & 0x1FFFFFFFu;
    if (g_biggerBag && inside(kMenuItemOpenState, getRegU32(ctx, 31)) &&
        static_cast<int32_t>(getRegU32(ctx, 4)) == kFirstSection && section >= kFirstSection &&
        section <= kLastSection && grid)
    {
        setRegS32(ctx, 4, kFirstSection + 1);
        rdram[grid + kGridTab] = kFirstSection + 1;
    }
    g_menuItemSelectTab(rdram, ctx, runtime);
}

// The list ItemIsEquippableBy's caller is building, by its return address,
// and how many it holds.
unsigned equippableRoom(uint32_t returnPc)
{
    if (!g_biggerBag)
        return 0u;
    if (inside(kMenuEquipCandidatesFill, returnPc))
        return kMenuEquipCandidates;
    if (inside(kBattleWeaponListBuild, returnPc) && std::strcmp(rg::overlay::residentName(), "normal") == 0)
        return kBattleWeapons;
    return 0u;
}

void iterInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (equippableRoom(getRegU32(ctx, 31)) != 0u)
        g_equippable = 0u;
    g_iterInit(rdram, ctx, runtime);
}

void capEquippable(uint8_t *, R5900Context *ctx, PS2Runtime *, uint32_t room)
{
    if (getRegU32(ctx, 2) != 0u && ++g_equippable > room)
        setReturnS32(ctx, 0);
}

void itemIsEquippableBy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const unsigned room = equippableRoom(getRegU32(ctx, 31));
    if (room == 0u)
    {
        g_itemIsEquippableBy(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_itemIsEquippableBy, rdram, ctx, runtime, &capEquippable, room);
}

// UiFindWidget: the layout's widgets, a list of nodes (widget, -, next).
uint32_t findWidget(const uint8_t *rdram, uint32_t layout, uint32_t id)
{
    uint32_t node = layout ? readU32(rdram, layout) & 0x1FFFFFFFu : 0u;
    for (unsigned n = 0; node && n < 4096u; ++n)
    {
        const uint32_t widget = readU32(rdram, node) & 0x1FFFFFFFu;
        if (!widget)
            return 0u;
        if ((readU32(rdram, widget) & 0xFFFFu) == id)
            return widget;
        node = readU32(rdram, node + kWidgetNext) & 0x1FFFFFFFu;
    }
    return 0u;
}

// The sprites the tab row calling writes, or 0.
uint32_t tabRowSprites(uint32_t returnPc)
{
    if (inside(kMenuDrawItemTabs, returnPc))
        return kItemTabSprites;
    if ((inside(kShopSellTabs, returnPc) || inside(kThinkMenuTabs, returnPc)) &&
        std::strcmp(rg::overlay::residentName(), "normal") == 0)
        return kSellTabSprites;
    return 0u;
}

// (layout, widget, x, y, ...): a tab row's widget gets room for its tabs.
void uiSetSpritePosSizeUv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t room = g_biggerBag ? tabRowSprites(getRegU32(ctx, 31)) : 0u;
    const uint32_t widget =
        room ? findWidget(rdram, getRegU32(ctx, 4) & 0x1FFFFFFFu, getRegU32(ctx, 5) & 0xFFFFu) : 0u;
    const uint32_t list = widget ? readU32(rdram, widget + kWidgetSprites) & 0x1FFFFFFFu : 0u;
    const bool ours = std::any_of(g_tabLists.begin(), g_tabLists.end(),
                                  [list](const auto &copy) { return copy.second == list; });
    if (list && !ours)
    {
        uint32_t count = 0u;
        while (count < kSpritesMax && readS16(rdram, list + count * kSpriteSize) != -1)
            ++count;
        if (count >= 2u && count < room)
        {
            auto copy = std::find_if(g_tabLists.begin(), g_tabLists.end(),
                                     [list](const auto &entry) { return entry.first == list; });
            uint32_t mine = copy != g_tabLists.end() ? copy->second : 0u;
            if (!mine)
            {
                mine = runtime->guestMalloc((room + 1u) * kSpriteSize);
                if (mine)
                    g_tabLists.emplace_back(list, mine);
            }
            if (mine)
            {
                std::memcpy(rdram + mine, rdram + list, count * kSpriteSize);
                for (uint32_t i = count; i < room; ++i)
                    std::memcpy(rdram + mine + i * kSpriteSize, rdram + mine + (i - 2u) * kSpriteSize, kSpriteSize);
                std::memset(rdram + mine + room * kSpriteSize, 0, kSpriteSize);
                writeS16(rdram, mine + room * kSpriteSize, -1);
                writeS32(rdram, widget + kWidgetSprites, static_cast<int32_t>(mine));
                std::fprintf(stderr, "[rg] bigger bag: widget 0x%X's tab sprites moved (%u, now %u)\n",
                             readU32(rdram, widget) & 0xFFFFu, count, room);
            }
        }
    }
    g_uiSetSpritePosSizeUv(rdram, ctx, runtime);
}

bool tableIsTheGames(const uint8_t *rdram)
{
    return readS32(rdram, kBagPages) == kGameBagEnd / kPageSlots && readS32(rdram, kBagLast) == kGameBagEnd - 1 &&
           readS32(rdram, kSlotsAfterBag[0]) == kGameBagEnd && readS32(rdram, kPoolSize) == kGamePoolSize;
}

// Before the game starts: the table, and the pool's size, which PartyInit
// reads.
void patchTable(uint8_t *rdram)
{
    writeS32(rdram, kBagPages, readS32(rdram, kBagPages) + 1);
    writeS32(rdram, kBagLast, readS32(rdram, kBagLast) + kPageSlots);
    for (uint32_t address : kSlotsAfterBag)
        writeS32(rdram, address, readS32(rdram, address) + kPageSlots);
    writeS32(rdram, kPoolSize, kBigPoolSize);
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (envOn("RG_STACK_LIMIT_99"))
    {
        setStackLimit99(true);
    }
    if (envOn("RG_FULL_BAG_TO_STORAGE"))
    {
        setFullBagToStorage(true);
    }
    if (const char *bigger = std::getenv("RG_BIGGER_BAG"); bigger && *bigger)
    {
        setBiggerBag(*bigger != '0');
    }
    if (!rg::timing::hookFunction(runtime, kItemDefGetStackLimit, &itemDefGetStackLimit, g_itemDefGetStackLimit) ||
        !rg::timing::hookFunction(runtime, kInvAddItemCore, &invAddItemCore, g_invAddItemCore) ||
        !rg::timing::hookFunction(runtime, kInvAddItemRepeat, &invAddItemRepeat, g_invAddItemRepeat))
    {
        std::fprintf(stderr, "[rg] inventory options: some hooks are missing, see above\n");
    }
    if (!g_biggerBagPreferred.load(std::memory_order_relaxed))
    {
        return;
    }
    // The hooks first, the table only if they all took: they do nothing
    // until then.
    uint8_t *rdram = runtime.memory().getRDRAM();
    if (!tableIsTheGames(rdram))
    {
        std::fprintf(stderr, "[rg] bigger bag: the item table is not the one expected, left as it is\n");
        return;
    }
    if (!rg::timing::hookFunction(runtime, kPartySerialize, &partySerialize, g_partySerialize) ||
        !rg::timing::hookFunction(runtime, kPartyDeserialize, &partyDeserialize, g_partyDeserialize) ||
        !rg::timing::hookFunction(runtime, kPartyInit, &partyInit, g_partyInit) ||
        !rg::timing::hookFunction(runtime, kPartyMenuCursorSet, &partyMenuCursorSet, g_partyMenuCursorSet) ||
        !rg::timing::hookFunction(runtime, kMenuItemSectionEnter, &menuItemSectionEnter, g_menuItemSectionEnter) ||
        !rg::timing::hookFunction(runtime, kMenuItemSelectTab, &menuItemSelectTab, g_menuItemSelectTab) ||
        !rg::timing::hookFunction(runtime, kItemIsEquippableBy, &itemIsEquippableBy, g_itemIsEquippableBy) ||
        !rg::timing::hookFunction(runtime, kIterInit, &iterInit, g_iterInit) ||
        !rg::timing::hookFunction(runtime, kUiSetSpritePosSizeUv, &uiSetSpritePosSizeUv, g_uiSetSpritePosSizeUv))
    {
        std::fprintf(stderr, "[rg] bigger bag: some hooks are missing, the bag stays as it is\n");
        return;
    }
    patchTable(rdram);
    g_biggerBag = true;
    std::fprintf(stderr, "[rg] bigger bag: 7 pages, 280 slots\n");
}

void setStackLimit99(bool on)
{
    g_stackLimit99.store(on, std::memory_order_relaxed);
}

void setFullBagToStorage(bool on)
{
    g_fullBagToStorage.store(on, std::memory_order_relaxed);
}

unsigned takeSentToStorage()
{
    return g_sentToStorage.exchange(0u, std::memory_order_relaxed);
}

void setBiggerBag(bool on)
{
    g_biggerBagPreferred.store(on, std::memory_order_relaxed);
}

bool biggerBag()
{
    return g_biggerBag;
}

} // namespace rg::inventory
