#!/usr/bin/env python3
"""Fetch Rogue Galaxy's RetroAchievements set (game 2671) into a JSON file.

usage: ra_fetch.py [--user NAME] [--out FILE] [--game ID] [--save-login]

The RetroAchievements server only hands out an achievement's logic (its
memory conditions) to a signed-in account, so this asks for the account's
password (typed here, never stored, never printed), signs in, downloads the
set and writes it. Without --save-login only the set is written: not the
password, not the token. With it, the connection token is also kept in
ra/login.json (mode 600, next to the set) so the game can sign in by itself;
the password is still never kept. Deleting that file signs the game out.

The default output is outside the repository, in the game's settings folder:
the set is RetroAchievements' work and does not go into git.
"""
import argparse
import getpass
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

API = "https://retroachievements.org/dorequest.php"
GAME = 2671


def call(params, post=False):
    data = urllib.parse.urlencode(params)
    if post:
        req = urllib.request.Request(API, data=data.encode(), method="POST")
    else:
        req = urllib.request.Request(API + "?" + data)
    req.add_header("User-Agent", "RogueGalaxyRecomp-ra-fetch/1")
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        # The server answers a refusal with a status code and a JSON body
        # saying why: show that rather than a traceback.
        try:
            return json.load(error)
        except ValueError:
            return {"Success": False, "Error": "HTTP %d" % error.code}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--user", help="RetroAchievements user name")
    parser.add_argument("--game", type=int, default=GAME)
    parser.add_argument("--out", help="output file")
    parser.add_argument("--save-login", action="store_true",
                        help="keep the connection token for the game (ra/login.json, mode 600)")
    args = parser.parse_args()

    user = args.user or input("RetroAchievements user: ").strip()
    password = getpass.getpass("Password (not shown, not kept): ")
    login = call({"r": "login2", "u": user, "p": password}, post=True)
    del password
    if not login.get("Success"):
        print("Sign-in refused: %s" % login.get("Error", "unknown error"), file=sys.stderr)
        return 1
    token = login["Token"]

    patch = call({"r": "patch", "g": args.game, "u": user, "t": token})
    if not patch.get("Success", True) or "PatchData" not in patch:
        print("Download refused: %s" % patch.get("Error", "unknown error"), file=sys.stderr)
        return 1
    data = patch["PatchData"]

    out = args.out or os.path.join(
        os.path.expanduser("~/.local/share/RogueGalaxy/ra"), "game_%d.json" % args.game)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    if args.save_login:
        login_path = os.path.join(os.path.expanduser("~/.local/share/RogueGalaxy/ra"), "login.json")
        os.makedirs(os.path.dirname(login_path), exist_ok=True)
        fd = os.open(login_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump({"user": login.get("User", user), "token": token}, f)
        os.chmod(login_path, 0o600)
        print("Sign-in kept in %s" % login_path)
    count = len(data.get("Achievements", []))
    print("%s: %d achievements written to %s" % (data.get("Title", "?"), count, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
