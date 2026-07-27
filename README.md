# wxl-gear-recolor

> **PROJECT FROZEN** — development paused until textureitem→3Ditem conversion is solved.
> Details below.

## Why this project is frozen (Jul 2026)

The project is frozen until I figure out a bunch of stuff. Here's what's going on:

What you find on GitHub is **perfectly working** if you only use it on yourself and do not
count it as part of transmogging. It attaches to the **slot**, not the specific item —
meaning the tint set up on chest will work on all chests and so on. However it might or
might not tint other players' and other NPCs' items from time to time (it's a bug).

I tried to merge it with the tmog system: make tints **player-specific** and
**item-specific**, integrate it in the transmog UI, have the server save the tint per-item,
and build client-server queries to know which item should have which tint. It worked, but
with some stupid recurring bugs that I can't manage to fix.

To understand those bugs you first have to know that gear is divided into two types:

- **3D items** — actual 3D models added on top of the character (head, shoulder,
  weapons). I call these *3Ditems*.
- **Texture items** — only texture glued straight to the character's model, often
  forcing that model to change its shape (chest, waist, legs, boots, and so on). I call
  these *textureitems*.

There are bugs involving 3Ditems and bugs involving textureitems.

**3Ditems** will share their tint to nearby NPCs. This is fixable but re-bugs constantly
as I keep trying to fix what's next (the real deal).

**Textureitems** are the real problem. Because they edit the player's model, changing
tint of one of those items can change the tint of another piece of the model — sometimes
even a piece that isn't gear per se, such as the player's skin. Applying a tint can also
apply it to other textureitems on nearby players' gear. This is visual only (on their
client, they don't see this), but it's gamebreaking and fucking unfixable.

The idea now is to make a script that converts all textureitems into 3Ditems — which
again also have some bugs, but they're minor, fixable, and much easier to manage. Once
that is fine, I can rework the whole tint module to work for all slots like it does
currently for head/shoulder/weps, and then it's gg.

That script isn't easy to do. I'm wasting tens of millions of tokens into it and I can't
manage to make it working. **If you know how to do this, hit me up.** Once that is done
I'm convinced I can finish the project in a matter of hours. Finished project = you install
this on your server and you literally have tints on top of tmogs.

Last thing: even if you were able to fix the itemtexture bugs I can't manage to fix, it
would still mean that the system would not work on backported items from later xpacs which
do involve 3D models on slots where WotLK only had textureitems. So regardless of your
debugging skill, the path of converting textureitems to 3Ditems is still the way to go.

---

Client-side gear recolor for **WarcraftXL** (WotLK 3.3.5a client DLL / module system).

Pick a color per equipment slot (`/recolor`) — **Solid** or **Selective** (from→to color rules). No server DB changes.

## Contents

| Path | What |
|---|---|
| `module/wxl-gear-recolor/` | WarcraftXL DLL module (`scripts/wxl-gear-recolor/`) |
| `addon/WXLRecolor/` | Standalone WotLK addon (`Interface/AddOns/WXLRecolor/`) |

## Install

1. Copy `module/wxl-gear-recolor` into your WarcraftXL tree as `wxl-core/scripts/wxl-gear-recolor/`.
2. Rebuild / redeploy `WarcraftXL.dll` into the client folder.
3. Copy `addon/WXLRecolor` into `Interface/AddOns/`.
4. In-game: `/recolor`.

Opt-out: create `WarcraftXL_gear-recolor.disable` next to `Wow.exe`.

## How it works (short)

- **Body armor** (chest, legs, hands, …): colorize paletted `Item\TextureComponents\…` before CharComponent paste.
- **3D attachments** (head, shoulders, weapons): live pixel-shader colorize on ObjectComponent draw.
- Tint math: keep luminance (shading), apply picked RGB as chroma (selective uses soft match + neighbor cleanup on OC).
- Enter-world prefers natural paste tint (char-select quality); logout flushes TextureCache backups so glue/relog do not reuse stale pointers.
- Glue scoping: body tint is sticky/one-shot on the local CharacterComponent only; 3D gear (head/shoulder/weapon) uses the same sticky root on char-select and the live player model in-world.

## License

GPLv3 (same family as WarcraftXL).
