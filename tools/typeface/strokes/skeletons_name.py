"""Each character of the names' font (Rogue Letters Names: the speaker's
name over the dialogue box, the names over heads) as pen strokes, in its
16x18 cell, in skeletons.py's notation.

The console's names font is not the message font made smaller: heavier and
narrower, with a two-storey a, a 4 closed at the top, a 9 whose stem curves
round to the left, a J with a bar on top, an M with upright legs, and d and
u without a foot. Those are drawn here from its pixels (dump_cells.py
--font name); the others started as the message font's skeletons carried
from its ink box to the names' (the fit moves them from there). The comma,
apostrophe and quotes are the message font's drawn "9", its head the size
of this font's period. Where the fit left them is strokes_name.json.
"""

SKELETONS = {
    # ------------------------------------------------------------ digits
    "0": ["5.2,1.7 7.9,3.7 8.4,7.8 7.4,11.6 4.8,13.8 2.0,12.0 1.5,7.8 2.4,3.5 z"],
    "1": ["0.6,3.5 4.5,0.9c 4.5,14.8"],
    # The diagonal comes down steep to the base, on row 13 as the console's
    # (a sharp corner lower down made an odd foot).
    "2": ["1.4,4.2 2.3,2.3 4.5,1.6 6.8,2.4 7.5,4.4 6.8,6.6 4.4,8.8 2.2,10.8 1.6,13.0c 8.6,13.0"],
    # One stroke turning back at the middle (two ends meeting there stood
    # a knob out).
    "3": ["1.7,3.5 2.8,2.0 5.0,1.6 7.2,2.2 7.6,4.1 6.2,6.4 3.2,7.1c 6.4,7.3 8.2,9.4 8.0,11.7 5.8,13.8 3.2,13.8 "
          "1.0,12.0"],
    # Closed: the stem goes up to the top, where the diagonal leaves it.
    "4": ["6.9,14.6 6.9,1.4c 0.9,10.3c 9.8,10.3"],
    # One stroke: the stem down to row 7, the bowl leaving it up to the
    # right at a corner, as the console's.
    "5": ["8.4,1.5 2.0,1.5c 1.8,7.4c 4.4,6.3 6.8,6.8 7.9,9.4 7.4,12.0 5.2,13.2 2.8,13.0 1.4,11.4"],
    # The top curls round to the right, its end turned down.
    "6": ["7.8,3.6 6.6,2.0 4.6,1.7 2.8,2.8 1.9,5.4 1.8,9.6j",
          "4.8,7.2 7.4,8.5 8.1,11.1 7.2,13.7 4.8,14.7 2.4,13.7 1.5,11.1 2.2,8.5 z"],
    "7": ["0.5,1.9 7.7,1.9c 5.8,5.5 4.1,9.8 2.9,15.8h"],
    "8": ["4.8,1.7 7.2,2.4 7.6,4.1 6.4,6.0 4.7,7.3 2.8,6.0 1.9,4.1 2.6,2.3 z",
          "4.7,7.4 7.2,8.7 8.1,11.3 7.2,13.7 4.8,14.7 2.4,13.7 1.5,11.3 2.4,8.7 z"],
    # The bowl, and the stem curving down and round to the left.
    "9": ["4.6,1.6 6.9,2.6 7.6,5.0 7.0,7.4 4.6,8.3 2.4,7.6 1.6,5.0 2.4,2.6 z",
          "7.8,5.0j 8.0,8.6 7.5,11.2 5.8,13.0 3.6,13.2 1.8,11.8"],
    # ------------------------------------------------------------ capitals
    "A": ["1.0,13.8h 5.3,1.8c 9.2,13.8h",
          "2.5,9.1j 7.6,9.1j"],
    # B, D, P, R: the stem and the bowl one stroke, square at the corners
    # as the console's (two round ends met there in a knob); the B's bowls
    # each a closed loop, as the D's (a stroke coming back up the stem to
    # its own start filled its counter in the outline).
    "B": ["2.2,2.5c 5.0,2.4 7.2,3.0 7.8,4.5 6.8,6.2 4.7,7.0 2.2,7.1c z",
          "2.2,7.1c 5.5,7.3 7.8,8.3 8.4,10.0 7.7,11.7 5.2,12.9 2.2,12.9c z"],
    "C": ["10.2,4.4 8.7,2.7 5.9,2.3 3.0,3.6 1.5,6.6 1.5,9.9 2.8,12.5 5.7,14.0 8.9,13.5 10.9,10.9"],
    "D": ["2.4,2.5c 5.5,2.5 8.5,3.8 10.2,6.8 10.0,9.4 8.1,11.7 5.2,12.9 2.4,12.9c z"],
    "E": ["8.0,2.3 1.8,2.5c 1.8,12.9c 8.7,12.9",
          "1.8,7.0j 7.6,7.0"],
    "F": ["8.9,2.3 2.1,2.5c 2.1,13.7",
          "2.1,7.0j 8.4,7.0"],
    # One stroke: the bowl comes round into the stem (u), up to the bar
    # (two strokes met at the bottom right and stood a corner out).
    "G": ["8.6,4.2 7.5,2.7 5.0,2.3 2.6,3.6 1.3,6.6 1.3,9.9 2.4,12.6 4.9,14.0 7.6,13.6 9.6,11.6u 9.7,7.6c 6.2,7.6"],
    "H": ["2.0,1.8 2.0,13.8",
          "7.9,1.8 7.9,13.8",
          "2.0,7.0j 7.9,7.0j"],
    "I": ["1.4,1.8 6.5,1.6",
          "4.1,1.8j 4.1,13.0j",
          "1.4,13.0 6.8,13.0"],
    # A bar on top; the hook flat along the baseline.
    "J": ["2.0,2.0 9.0,2.0",
          "5.7,2.0j 5.7,10.4t 5.0,12.4 3.4,12.9 1.2,12.7"],
    "K": ["1.5,1.8 1.5,13.8",
          "8.2,1.8h 1.8,7.7j",
          "3.5,7.0j 8.7,13.8h"],
    "L": ["3.1,1.8 3.1,13.0c 10.0,13.0"],
    # One stroke: up the left leg, a short way along the top, down the V
    # (it reaches two rows above the baseline), up and along, down the
    # right leg; every corner at most 110 degrees, so cut square, and the
    # top one height (the V apart, its ends stood above the legs' tops;
    # turning straight from leg to V, the corners were cut off aslant).
    "M": ["1.8,13.9 1.8,1.1c 2.6,1.1c 6.9,12.4c 10.8,1.1c 11.6,1.1c 11.6,13.9"],
    # As the M: a short way along the top and the bottom at the sharp
    # corners, cut square there (turning straight, they were cut aslant).
    "N": ["1.3,13.8 1.3,1.8c 2.1,1.8c 6.9,13.7c 7.7,13.7c 7.7,1.7"],
    "O": ["5.3,2.5 8.3,4.1 9.3,8.1 7.9,12.2 5.0,13.9 2.2,12.4 1.3,8.2 2.5,4.0 z"],
    "P": ["2.4,13.9 2.4,2.6c 4.5,2.4 7.7,3.2 8.4,5.1 6.9,7.0 4.2,7.7 2.4,7.7"],
    "Q": ["5.3,2.4 8.3,4.0 9.3,7.8 7.9,11.7 5.0,13.4 2.2,11.9 1.3,7.9 2.5,3.8 z",
          "4.9,10.0 9.4,14.9"],
    "R": ["2.2,13.8 2.2,2.5c 4.2,2.4 7.0,3.0 7.7,4.7 6.7,6.7 4.5,7.3 2.2,7.4",
          "3.7,7.6j 5.5,10.5 8.3,13.8h"],
    "S": ["9.1,4.3 8.0,2.7 5.1,2.3 2.5,3.0 1.7,5.1 3.3,6.9 6.0,7.9 8.2,9.4 8.6,11.7 7.2,13.5 4.4,14.1 2.0,13.4 0.9,12.0"],
    "T": ["1.3,1.9 8.7,1.6",
          "5.1,1.8j 5.1,13.7"],
    "U": ["2.3,1.8 2.3,10.4 3.5,13.0 6.3,13.9 9.2,13.2 10.7,10.9 10.7,1.8"],
    "V": ["0.8,1.8h 5.8,13.7c 10.8,1.6h"],
    "W": ["0.9,1.0h 3.9,13.7c 6.9,1.6c 10.3,13.7c 13.5,1.0h"],
    "X": ["1.8,1.8h 10.1,13.7h",
          "9.7,1.8h 1.7,13.7h"],
    "Y": ["1.5,1.8h 5.5,8.2c 5.5,13.7",
          "9.7,1.8h 5.5,8.0j"],
    "Z": ["1.0,1.9 8.7,1.9c 0.8,12.5c 9.7,12.5"],
    # ------------------------------------------------------------ lowercase
    # Two storeys: the top arc coming down into the stem (u), the stem with
    # a small foot to the right, and the bowl hanging from the stem. (The
    # arc and the stem apart left a notch at the shoulder.)
    # The stem comes round into a small foot whose end turns up a little
    # (the user, 2026-11-26: "C menor"), the stroke held to it (PULL_FOR), the
    # bowl free.
    "a": ["1.8,6.7 3.2,5.2 5.2,5.0 6.6,5.8 6.9,7.4u 6.9,11.5t 7.5,12.7 8.3,12.5",
          "6.9,8.4j 4.6,8.5 2.4,9.4 1.6,10.9 2.6,12.4 4.6,12.7 6.6,11.8j"],
    "b": ["1.5,0.9 1.5,14.3",
          "1.8,5.6j 4.0,5.2 6.2,5.8 7.1,8.4 6.8,10.9 5.2,12.3 1.8,12.4j"],
    "c": ["8.2,7.3 7.0,5.6 5.0,5.4 2.6,6.6 1.5,9.7 2.4,12.6 4.8,13.8 7.0,13.2 8.2,11.3"],
    # The stem straight down to the baseline.
    "d": ["7.5,0.9 7.5,13.9",
          "7.4,5.6j 5.0,5.2 2.6,6.0 1.4,8.6 1.8,11.0 3.6,12.3 7.4,12.3j"],
    # The bar comes round into the bowl (t), with room for the turn: a
    # corner there stood a point out at the right (the user, 2026-11-26),
    # and a turn in less than a pixel a dent.
    # Its end runs along the bottom and turns up a little at the last, as
    # the t's (the user, 2026-11-26), held (PULL_FOR).
    "e": ["1.3,9.6 6.8,8.7t 7.8,7.6 7.3,6.0 5.6,5.2 3.6,5.5 2.0,7.1 1.3,9.5 1.7,11.6 3.4,12.9 5.6,13.1 7.2,12.6 8.2,11.8"],
    "f": ["3.5,13.7 3.5,3.3 4.1,1.8 5.3,1.5 6.3,3.0",
          "1.0,5.8 6.7,5.8"],
    # Read from the names' pixels, fewer nodes (two side by side bent the
    # bowl's left and the tail's end).
    "g": ["7.4,5.8j 5.0,4.8 2.6,5.6 1.5,8.2 2.6,10.8 4.8,11.6 7.2,10.6j",
          "7.6,4.3 7.6,12.0t 6.4,14.2 4.0,14.9 1.4,13.6"],
    "h": ["2.1,1.8 2.1,13.8",
          "2.3,7.7j 3.5,6.0 5.1,5.9 6.3,6.8 6.6,8.6 6.6,13.8"],
    "i": ["dot 3.67,2.22 0.812,1.056",
          "3.7,5.1 3.7,13.7"],
    "j": ["dot 3.67,2.25 0.667,0.867",
          "3.6,5.2 3.6,13.0t 3.2,14.9 2.3,15.5 1.3,15.1"],
    "k": ["1.7,0.8 1.7,13.8",
          "7.4,4.6h 1.9,8.7j",
          "3.4,8.4j 8.4,13.7h"],
    # An L: a straight stem and foot, the console's (the fit bent a smooth
    # corner's foot up to the right); the foot a little shorter than the
    # console's (the user, 2026-11-26), held (PULL_FOR).
    "l": ["1.8,1.0 1.8,12.6c 4.0,12.6"],
    # The last arch comes down into a straight leg (u): with a free node at
    # its shoulder, the fit followed a faint pixel the console has right of
    # it and bent a kink there.
    "m": ["1.9,4.5 1.9,13.8",
          "2.0,7.0j 3.7,5.3 5.6,5.2 6.9,6.2 7.2,8.0 7.2,13.8",
          "7.3,7.0j 9.0,5.4 10.9,5.5 11.9,7.6u 11.9,13.8"],
    "n": ["1.4,4.5 1.4,13.8",
          "1.5,7.2j 2.9,5.3 4.6,5.2 6.1,6.2 6.4,8.2 6.4,13.8"],
    "o": ["5.0,5.4 7.6,6.8 8.3,9.5 7.4,12.2 4.6,13.7 2.0,12.4 1.5,9.7 2.4,6.8 z"],
    "p": ["1.5,4.5 1.5,16.5",
          "1.8,5.4j 4.2,5.1 6.2,5.8 7.0,8.4 6.4,10.8 4.4,12.0 1.8,11.6j"],
    "q": ["7.3,4.9 7.2,16.5",
          "7.6,5.6j 5.0,5.0 2.6,5.8 1.4,8.4 2.0,10.8 4.2,12.1 7.6,11.6j"],
    "r": ["1.5,4.5 1.5,13.8",
          "1.7,7.3j 2.8,5.7 4.4,5.2 5.6,5.5 6.1,6.4"],
    "s": ["7.7,7.4 7.8,6.2 6.7,5.5 5.1,5.4 3.7,5.9 3.0,7.1 3.3,8.4 5.1,9.3 7.0,10.1 8.0,11.6 7.4,13.4 5.1,14.1 3.1,13.5 2.2,11.9"],
    # The stem rounding into a hook whose end turns up a little (the user,
    # 2026-11-26; the console's lies flat), held to that shape (PULL_FOR).
    "t": ["4.0,1.6 4.0,11.2t 4.9,12.6 6.0,12.8 7.0,12.1",
          "1.0,5.7 7.2,5.7"],
    # The stem straight down to the baseline.
    "u": ["1.4,4.5 1.4,10.1 2.1,12.8 3.7,13.7 5.3,13.0 6.2,11.0j",
          "6.9,4.4 6.9,13.9"],
    "v": ["1.4,4.5h 4.5,13.7c 7.6,4.5h"],
    "w": ["1.4,4.7h 4.2,13.7c 6.9,4.8c 9.6,13.7c 12.5,4.5h"],
    "x": ["1.6,3.6h 7.6,14.6h",
          "7.2,3.6h 1.3,14.6h"],
    # The long leg rounding into a straight foot to the left (t, u).
    "y": ["1.7,4.4h 5.0,12.3j",
          "7.7,4.4h 4.9,12.6t 3.6,14.5u 1.2,14.5"],
    "z": ["1.2,4.9 8.2,4.9c 1.0,12.5c 9.0,12.5"],
    # ------------------------------------------------------------ signs
    "!": ["1.2,1.1c 3.4,1.1c 3.0,11.4c 1.5,11.4c f",
          "dot 2.30,13.32 1.147,1.147"],
    # Two apostrophes, their heads 3 pixels apart, as the console's.
    '"': ["comma 1.89,2.40 1.136,1.427",
          "comma 4.89,2.40 1.136,1.427"],
    "#": ["3.8,0.4h 2.4,14.7h",
          "7.0,0.4h 5.7,14.7h",
          "0.8,4.8 9.2,4.8",
          "0.4,9.7 8.7,9.7"],
    "$": ["8.5,3.9 7.5,2.2 5.3,1.6 2.7,2.0 1.5,3.9 2.4,5.8 5.1,7.0 7.8,8.2 8.9,10.2 8.0,12.6 5.3,13.4 2.5,13.1 1.1,11.6",
          "5.3,0.0 5.0,16.3"],
    # The rings as ovals of four nodes, held near them (PULL_FOR): with
    # seven free nodes the top one took a dent at its upper left.
    "%": ["3.4,1.5 5.3,4.4 3.4,7.3 1.5,4.4 z",
          "10.5,0.4 3.0,15.3",
          "10.7,8.6 12.4,11.3 10.7,14.1 9.0,11.3 z"],
    # Read from the names' pixels, held near them: the fit set two nodes of
    # the top loop side by side and cut a corner into its upper left.
    "&": ["9.9,12.9 7.4,10.9 5.0,8.6 3.2,6.2 2.3,3.9 3.0,2.2 4.5,1.7 5.9,2.3 6.3,3.8 5.5,5.5 3.6,7.4 1.8,9.2 "
          "1.4,11.0 2.2,12.4 3.8,13.0 5.8,12.6 7.4,10.9 8.7,7.3"],
    # The comma (below), its head on the console's, rows 1 to 3.
    "'": ["comma 2.09,2.40 1.136,1.427"],
    "(": ["4.5,0.1 2.6,3.6 1.6,8.5 2.8,13.6 4.9,16.3"],
    ")": ["1.5,0.1 3.7,3.9 4.9,8.5 3.7,12.9 1.4,16.3"],
    "*": ["3.8,0.2 3.8,8.1",
          "1.1,2.1 6.3,6.1",
          "6.3,2.1 1.1,6.1"],
    "+": ["5.4,4.3 5.4,14.7",
          "0.5,9.5 10.0,9.5"],
    # The comma as drawn: its head is the period's dot as fitted
    # (strokes_name.json), a tenth of a pixel right and down.
    ",": ["comma 2.09,13.21 1.136,1.427"],
    "-": ["1.4,10.0 6.6,10.0"],
    ".": ["dot 2.20,13.00 1.300,1.300"],
    "/": ["5.6,0.0h 1.1,16.6h"],
    ":": ["dot 2.20,6.02 1.192,1.192",
          "dot 2.20,13.35 1.192,1.192"],
    ";": ["dot 2.50,5.89 1.114,1.114",
          "comma 2.09,12.90 1.136,1.427"],
    "<": ["6.9,3.4 0.8,8.8c 7.0,14.2"],
    "=": ["0.5,6.1 8.5,6.1",
          "0.5,11.4 8.5,11.4"],
    ">": ["0.5,3.2 7.2,8.6c 0.5,14.2"],
    "?": ["1.1,4.4 2.0,2.3 4.1,1.4 6.3,1.9 7.4,3.9 6.7,5.8 4.9,7.4 3.8,8.8 3.6,11.1",
          "dot 4.14,13.32 1.147,1.147"],
    # Read from the names' pixels: the a's stem straight down (t), round
    # into the ring, which ends at the lower right; the a's bowl an arc from
    # the stem's top round to its foot, the stem its right side, as a d's
    # (an oval crossing the stem left a gap between the two counters). The
    # ring's tail ends short of the a's foot, a gap between them where the
    # console's closes (the user, 2026-11-26: ACCEPT).
    # Held near them (PULL_FOR).
    "@": ["8.5,4.4 8.5,9.0t 9.8,10.3 11.6,9.4 12.3,7.0 11.2,3.2 7.4,1.5 3.2,2.8 1.4,7.8 3.2,12.4 7.2,14.0 "
          "10.4,13.4 11.8,12.4",
          "8.5,4.8j 6.8,4.4 5.0,5.4 4.5,7.4 5.2,9.6 6.9,10.3 8.5,9.6j"],
    "[": ["5.9,1.8 2.3,1.8c 2.3,16.2c 5.9,16.2"],
    "]": ["0.6,1.8 4.7,1.8c 4.7,16.2c 0.7,16.2"],
    "_": ["0.0,15.5 9.0,15.5"],
    # Straight upright pieces (u, t) between the curled ends and the point
    # (more free nodes made them jagged).
    "{": ["4.0,1.5 2.5,3.2u 2.5,6.6t 1.3,8.3c 2.5,10.0u 2.5,13.4t 4.0,15.3"],
    "}": ["1.2,1.5 3.3,3.2u 3.3,6.6t 4.5,8.4c 3.3,10.2u 3.3,13.4t 1.2,15.3"],
    # The plus's stem on the bars' middle, held there (PULL_FOR: free, the
    # fit set it a quarter pixel right).
    "±": ["4.95,2.0 4.95,10.4",
          "0.8,6.0 9.1,6.0",
          "0.8,12.6 9.1,12.6"],
    "·": ["dot 7.00,8.00 1.520,1.760"],
    "×": ["1.3,2.3 8.7,12.5",
          "8.5,2.3 1.2,12.5"],
    "※": ["1.4,1.5 9.3,13.3",
          "9.1,1.3 1.1,13.3",
          "dot 5.50,2.13 0.943,0.943",
          "dot 1.26,7.24 0.943,0.943",
          "dot 9.43,7.24 0.943,0.943",
          "dot 5.34,12.27 0.943,0.943"],
}

# The A's apex at the default limit, cut flat as the console's (the message
# font's is pointed: skeletons.py); the 4's corner and the < and >'s points
# cut flatter, within the console's ink. As the message font's: the j's hook
# held to its shape, the s's nodes a little, and those of the glyphs whose
# nodes the fit bent into dents and kinks; the t's upturned end held as the
# j's, the a's foot and the e's end turned up, the %'s rings upright (the
# fit leant them to the right), the ±'s plus on its bars' middle (the
# console's sits a little right of it) and the l's shorter foot.
LIMIT_FOR = {"4": 1.2, "<": 1.5, ">": 1.5}
PULL_FOR = {"j": 1.0, "s": 0.05, "%": 0.5, "±": 100.0, "l": 5.0, "&": 0.05, "@": 0.05,
            "a": [1.0, 0.05], "e": 0.5, "g": 0.05, "t": 1.0, "y": 0.05, "{": 0.05}
GLYPH_FOR = {"×": "multiply"}

# Glyphs drawn as another's mirror image (stroke_fit.mirror_step): the
# brackets, parentheses, braces and angle brackets each a pair alike (fitted
# apart, they came out a little unlike each other; the user, 2026-11-26).
MIRROR = {"]": "[", ")": "(", "}": "{", ">": "<"}

# Where the user chose a drawing the console's glyph does not have: the
# checks (check_glyphs.py) of these kinds let the glyph be.
ACCEPT = {"@": {"counters"}}
