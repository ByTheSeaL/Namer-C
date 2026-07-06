# Namer-C

A **native Windows** rebuild of [Namer](https://github.com/ByTheSeaL/Namer) —
the little app that gives you name ideas for anything: code identifiers,
fictional characters and places, paper and system titles, products, projects.

Where the original is Python + PySide6 (a ~48 MB bundled download), this is
plain **C++ against the Win32 API**. The result is a single self-contained
`Namer.exe` under 1 MB with no runtime to install and no DLLs to ship.

![Namer](assets/icon.ico)

## What it does

- Describe the thing you want to name on the left; pick a **context**
  (Code, Fiction, Paper / Technical, Product / Project, General).
- Pick any **OpenRouter** model from the searchable dropdown (the live model
  list loads in the background; tick **Free only** to see just the `:free`
  models). Recently used models float to the top and are remembered between
  runs.
- Click **Generate** for a table of names, each with a short rationale
  (hover a row for the full text).
- **Double-click** a name — or right-click → **Show similar** — to iterate on
  it. Right-click → **Re-prompt with this…** to fold a name back into your
  description and refine.
- Walk your result rounds with the **◀ / ▶** history buttons. Errors (e.g. a
  free model rate-limiting you) show in the status line without disturbing
  your history.

It shares the exact config location and files with the Python Namer
(`%APPDATA%\Namer\`), so your API key and preferences carry over.

## Setup

You need an [OpenRouter](https://openrouter.ai/keys) API key. Either:

- Set the `OPENROUTER_API_KEY` environment variable, or
- Open **File → Settings** in the app and paste your key.

## Building

### With Visual Studio / MSVC (what CI uses)

```powershell
cmake -B build -S .
cmake --build build --config Release
# -> build/Release/Namer.exe
```

Requires CMake 3.15+ and the Visual Studio C++ toolchain.

### With MinGW-w64 (incl. cross-compiling from Linux)

```sh
cmake -B build -S . -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=... # or run the compiler directly:

x86_64-w64-mingw32-windres -Ires res/namer.rc -O coff -o namer_res.o
x86_64-w64-mingw32-g++ -std=c++17 -municode -O2 \
  src/main.cpp namer_res.o -o Namer.exe \
  -lcomctl32 -lwinhttp -lshlwapi -lgdi32 \
  -static -static-libgcc -static-libstdc++ -mwindows
```

## Releasing

Tag a version and push; GitHub Actions builds `Namer.exe` with MSVC and
attaches it to a new release:

```sh
git tag v0.1.0
git push origin v0.1.0
```

## How it's built

Pure Win32 — no framework. The whole app is a handful of headers:

| File | Role |
|------|------|
| `src/main.cpp` | Window, controls, layout, menus, history, threading |
| `src/llm.hpp` | OpenRouter client over **WinHTTP** |
| `src/json.hpp` | Tiny dependency-free JSON parser/serializer |
| `src/prefs.hpp` | Last/recent models, JSON in `%APPDATA%\Namer` |
| `src/util.hpp` | UTF-8 ⇄ UTF-16 and file/path helpers |

OpenRouter calls run on background threads (native `CreateThread`) and post
their results back to the UI thread with `PostMessage`. In-flight requests are
soft-cancelled by bumping a sequence counter, so stale replies are ignored.

## License

MIT — see [LICENSE](LICENSE).
