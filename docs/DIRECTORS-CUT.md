# The Japanese Director's Cut

Rogue Galaxy came out again in Japan as a Director's Cut (SCPS-17013),
which changed the battles: the enemies, their groups, their attacks and
the allies' AI. The US release, which this project plays, has none of it.
Three options bring it to the US release, each on its own, from your own
Japanese disc:

| Option | What it turns on |
|---|---|
| Director's Cut balance | The battles' numbers: [below](#balance) |
| Director's Cut enemy behavior | The enemies' attacks and AI: [below](#enemy-behavior) |
| Director's Cut party AI | The allies' AI: [below](#party-ai) |

They are in F1 > Settings > Game, under Battles and rewards, off by
default, and take effect at the next start. Each needs `Install-Undub.bat`
to have read your image of the Director's Cut
([INSTALL.md](INSTALL.md#undub-optional)); until then the menu greys them
out.

Where you notice it: early on, the balance shows mostly as more money and a
few smaller groups. The weaker enemies come at the Rosencaster Prison, and
the stronger ones are the optional Area Masters.

## Balance

**Smaller groups.** 85 of the game's 374 groups of enemies come smaller,
from Rosa to the extra areas; none comes bigger. The Cactulus group in Rosa
is 2 to 3 instead of 4 to 5, and many groups of 3 to 5 become 3 to 4.

**More money.** 75 enemies leave 1.2 to 1.9 times the money, about
1.5 times in the middle. Most of it is in the first planets: Rosa's first
enemies give nearly twice as much (the Baphu 38 to 48 instead of 20 to 25),
Juraika's 1.7 to 1.8 times, the Rosencaster Prison's and the Starship
Factory's 1.5 to 1.7 times. Experience stays the same.

**Weaker enemies.** The 19 kinds of enemy in the Rosencaster Prison and the
Starship Factory, on Zerard: a sixth to a fifth less health, an eighth less
attack and a tenth less defense. They take about a quarter fewer hits and
hurt about a quarter less (a Roller has 750 health instead of 900). Three
story fights have less health, attack and defense too: the Giants in
chapter 1, the Attack Walker in chapter 5 and Gale in chapter 6, though
Gale resists status effects and most elements more. So do two Mimics.

**Stronger Area Masters.** The 10 Area Masters, the optional level 99
bosses, all have more defense, and some more health and attack. The
hardest change is the Dark Messiah's: 22,000 health instead of 15,000, and
it takes about 2.6 times the hits and hits about 2.8 times harder.

**Knockdowns.** Bosses and boss parts are never knocked down by ordinary
hits; in the US release 47 of the 109 could be. The Burning Strike's last
hit and a few abilities take another path and still can, as on the
Japanese disc. Beyond that, 68 other enemies, most of the big ones (the
Gigants, the Hill Giant, the Red Horn, the Mimics), are harder to knock
down.

**Status effects.** A few story bosses resist poison, paralysis and the
like more, or entirely, and three boss attacks no longer poison or
paralyze.

**Smaller changes.** A few enemies drop other items, or drop theirs more
often, and a few are harder to stun.

**What stays.** The damage formula, experience, the party's levels and
numbers, and the weapons and items themselves are the same in both
releases. Enemy names stay as your text has them: the balance works with
the [European texts](INSTALL.md#european-texts-optional) too.

## Enemy behavior

**Their attacks.** The Director's Cut changed the files of 76 kinds of
enemy. In 56 of them it changed where and when their blows hit (how far a
blow reaches, how long it can hit), and for some the kind of blow, which
sets its strength and whether it breaks your guard; in the others, mostly
the size of the body. The Alacan's two attacks hit softer, the
Zombie's punches reach farther and hit for longer, and one of the Beast
Seed's skill attacks hits softer and no longer goes through your guard.

**Their AI.** About 70 kinds of enemy turn to face you before they attack
when you are off to their side, each at its own speed, and stand a moment
before trying again when a move of theirs fails. 22 kinds start their
attacks from farther away, among them the Mish, the Sand Tortoise and the
Cactulus. A few bosses' AI changed in smaller ways, among them the
Individer's and the Drill Rig's.

**Flying enemies.** The party's jump attacks keep homing in on a flying
enemy knocked out of the air, and on two kinds of jellyfish.

## Party AI

An ally the game plays for you, low on health (under a quarter), sometimes
falls back to the leader, steps back and guards for about two seconds:
about three times in ten when its AI picks what to do. The same for all
nine characters.

## The European release

The "PAL damage" players speak of is this rebalance: the European release
has most of it (the bosses' rule, the enemies' health, attack and defense,
the smaller groups, the boss attacks' effects and most of the enemies'
attacks), but not the money or the harder knockdowns. Its damage formula is
the US one, and none of it comes from its 50 Hz mode. These options take
the Japanese disc's numbers.

## Where it comes from

No file of the Japanese disc is in this project. `Install-Undub.bat` reads
your image: besides the voices and movies, it writes into
`disc\JA\balance` the four tables the balance changes (the Japanese
numbers, with your text's names) and the enemies' and allies' files the
Director's Cut changed, as that disc has them. With the European texts,
the install that comes second (`Install-Undub.bat` or
`Install-European.bat`) makes the tables for their languages. How it is
done: [DISC.md](DISC.md#the-japanese-disc-undub) and `src/dc_balance.h`.

What has been played with the options on is the early game; most of the
above comes from comparing the two discs' files. The comparison is with the
US release; the first Japanese release, from 2005, was not compared.
