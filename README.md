# patch_line_wine

| Before Patch | After Patch |
| :---: | :---: |
| <img width="1068" height="804" alt="Screenshot from 2026-09-25 10-42-12" src="https://github.com/user-attachments/assets/fed30c1d-dcfc-4147-9bcf-c84dfe517ad4" /> | <img width="1448" height="1056" alt="Screenshot from 2026-09-25 10-42-31" src="https://github.com/user-attachments/assets/440fc606-2d4d-4c1c-8276-407b1c6fb084" /> |
| <img width="1936" height="1506" alt="Screenshot from 2026-09-25 10-44-00" src="https://github.com/user-attachments/assets/82ed1a4c-1941-4faa-9c95-89ce6ac51db9" /> | <img width="1904" height="1464" alt="Screenshot from 2026-09-25 10-44-42" src="https://github.com/user-attachments/assets/fd5472be-a3f3-4819-84c1-296b1ae4dbe2" /> |

A single self-contained Python script that patches an existing [Bottles](https://usebottles.com/) (Flatpak) bottle so [LINE](https://line.me/) for Windows runs correctly under Wine.

It fixes two separate, unrelated problems that currently affect LINE on Wine:

1. **The drop-shadow "ghost window" bug** — LINE creates 8 auxiliary border/corner windows of its own (`shadow_side_*` / `shadow_corner_*`) that end up rendering as solid, always-on-top rectangles that cover other applications. This is **not** a Wine bug or a Qt bug — confirmed by grepping the actual Wine and Qt source trees for the class names involved, with zero matches in either — it's LINE's own closed-source shadow/resize-helper implementation, and exactly why it behaves this way under Wine isn't fully known.

2. **The `NO_SIGNATURE` error** — LINE 26.1.x and newer check whether Windows system DLLs are digitally signed before running. Wine's DLLs aren't signed by Microsoft, so LINE refuses to start. LINE's check only verifies that *a* signature is present, not that it's valid, so self-signing every DLL with a fake certificate satisfies it.

## What it does

Running the script against a chosen bottle:

1. Locates the bottle's own Wine binary.
2. Installs a custom `dwmapi.dll` proxy into the bottle's `system32` (the real one is renamed to `dwmapi_real.dll` and everything is still forwarded to it — DWM functionality is unaffected). The proxy hooks `CreateWindowExW` and `UpdateLayeredWindow`/`UpdateLayeredWindowIndirect` via [MinHook](https://github.com/TsudaKageyu/minhook) to detect LINE's shadow fragments by class name and force them fully transparent, while still letting them draw (so their window state stays valid and native resize keeps working).
3. Sets `HKCU\Software\Wine\DllOverrides\dwmapi` to `native,builtin`, which is required for Wine to actually load the file from disk instead of its own built-in `dwmapi.dll`. The `,builtin` fallback matters: the proxy is only ever placed in `system32` (64-bit), never in `syswow64` (32-bit), so any 32-bit process gracefully falls back to Wine's builtin instead of crashing with a missing-DLL error.
4. Generates a self-signed certificate (`O=Microsoft Corporation`, `CN=Microsoft Windows`) with `openssl`, then signs every `.dll` in both `system32` and `syswow64` with `osslsigncode`.

The `dwmapi.dll` proxy is embedded in the script as base64, so this is the only file you need.

## Requirements

Run on the **host**, not inside the bottle:

- [Bottles](https://usebottles.com/) installed via Flatpak (`com.usebottles.bottles`)
- Python 3.8+
- `openssl` and `osslsigncode` — the script will offer to install `osslsigncode` via `apt` if it's missing (Debian/Ubuntu-based distros only; install it yourself first on other distros)

## Usage

```bash
python3 patch_line_wine.py
```

Run this **before** installing LINE — LINE's installer itself performs the same signature check, so the `NO_SIGNATURE` fix needs to be in place first. The script will:

1. List your bottles and ask you to pick one.
2. Patch it (steps above).

After it finishes, download and run the LINE installer inside that bottle as usual.

## Notes

- Safe to re-run: it detects an existing `dwmapi_real.dll` backup and won't overwrite it with an already-patched `dwmapi.dll`, and re-signing an already-signed DLL is harmless.
- The DLL signing step **overwrites every `.dll` in `system32` and `syswow64`** in place. If you want to keep an unmodified copy of the bottle, back it up first.
- If you update or recreate the bottle (new runner, fresh bottle), you'll need to run the script again — a new bottle starts with unsigned, unpatched DLLs.
- **Changing the runner on an existing bottle also requires re-running this script.** A runner switch replaces the bottle's system DLLs with the new runner's (unsigned) copies, so `NO_SIGNATURE` will come back until you patch it again — this isn't limited to creating a brand new bottle.
- This only fixes running LINE under Wine; it doesn't fix anything on real Windows and has no effect there.

## Credits

The `NO_SIGNATURE` fix (self-signing DLLs with a fake certificate) is based on a guide originally posted by **YUKI.N (sonyandy123)** on the Bahamut forum:
https://forum.gamer.com.tw/Co.php?bsn=60030&sn=2550747

## License

Adul Tanthuvanit
