#pragma once

class PS2Runtime;

// What the bag holds, and where an item goes when it has no room.
//
// The party's items are one pool of objects (party+0x168, PartyInit
// 0x3CB6B0), each with its slot number at +6. The slot ranges come from a
// table at 0x4659E8: the bag 0..239 (six pages of 40), the transporter's item
// storage 240..559, then key items, Factory parts and the rest, and an
// overflow at 710..725. InvAddItemCoreMaybe (0x3C6540) adds an item to a kind
// of range: 0 the bag, 10 the bag then the storage, 9 the overflow. Weapons
// and the other items that do not stack take a slot each; those that do are
// held to ItemDefGetStackLimit (0x40E070) in the bag, all of a kind
// together: 20, 30 or 50 for some (healing items, boosts), 99 for the rest
// (0x465B60). A chest, a battle's spoils or a script that finds no room in
// the bag falls back to the overflow and opens the item menu to sort it out;
// with the overflow full too, the item is lost. The pool has 726 objects
// (0x465B50).
namespace rg::inventory
{

// After loadELF().
void install(PS2Runtime &runtime);

// Game option `stack_limit_99` (RG_STACK_LIMIT_99=1): every item that stacks
// is held to 99 in the bag.
void setStackLimit99(bool on);

// Game option `full_bag_to_storage` (RG_FULL_BAG_TO_STORAGE=1): an item added
// to the bag goes to the storage when the bag has no room for it (kind 10
// for 0), before the overflow.
void setFullBagToStorage(bool on);

// How many items went to the storage for want of room since the last call.
unsigned takeSentToStorage();

// Game option `bigger_bag` (RG_BIGGER_BAG=1), read by install(): the bag has
// a seventh page, slots 240..279, and every range after it moves up by 40.
// Saves keep the game's numbers: the seventh page goes in them as slots
// 726..765, which the game without the option keeps but does not show.
void setBiggerBag(bool on);

// Whether this run has the seventh page.
bool biggerBag();

} // namespace rg::inventory
