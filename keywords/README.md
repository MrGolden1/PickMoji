# Keyword packs

PickMoji's search keywords are extensible without rebuilding the app. At startup it
merges every `*.json` file found in:

1. `keywords/` next to `PickMoji.exe` — packs shipped with the app
2. `%APPDATA%/PickMoji/PickMoji/keywords/` — the user's own additions

into the search index. Keywords only ever **add** search terms — a pack can never
remove or break the built-in English/Persian search.

## Format

```json
{
  "language": "de",
  "keywords": {
    "😀": "lachen lächeln glücklich",
    "❤️": ["herz", "liebe"]
  }
}
```

- `language` is informational only; name the file after it (`de.json`, `fr.json`, …).
- Values may be a single space-separated string or an array of words.
- A bare top-level map without the `keywords` wrapper is also accepted.
- Variation selectors are ignored when matching, so `❤` and `❤️` both work as keys.
- Multiple files may add keywords to the same emoji; they accumulate.

`de.json.example` in this folder is a starter template — copy it, rename it to
`<lang>.json` and fill it in. (The `.example` suffix keeps it from being loaded.)
