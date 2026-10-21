# MetroGo: look and feel

## Palette (sampled from the supplied swatches, top to bottom)

| Colour | Hex | Used for |
|---|---|---|
| Sage | `#cdd9c5` | top bar, quiet borders |
| Pale sage | `#e5e8d5` | page background, input fill |
| Grey-green | `#a7afa0` | the "cheapest" accent |
| Terracotta | `#d49c63` | the "fewest changes" accent |
| Cream | `#faf6ea` | cards and panels |
| Peach | `#fcd4a1` | notices |
| Orange | `#f8ae45` | main buttons, selected choices, the "fastest" accent, the logo |

## Why some colours are NOT from the palette

All seven swatches are light. None is dark enough to read text on, and white text on orange reaches only 1.9:1 (WCAG asks
for 4.5:1). So these are darker shades of the SAME hues, found by computer so each one reaches the contrast needed:

| Token | Light | Dark | Used for |
|---|---|---|---|
| `--text` | `#27331c` | `#faf6ea` | body text, text on orange |
| `--muted` | `#526244` | `#cdd9c5` | secondary text |
| `--link` | `#784a1c` | `#fcd4a1` | links, "Go" in the name |
| `--ctrl` | `#687859` | `#798d68` | borders of inputs and buttons (3:1 needed) |
| `--fast-text` / `--cheap-text` / `--few-text` | `#8b5304` / `#486034` / `#845626` | the palette colours | titles of each option |
| `--danger` | `#932a1f` | `#f88375` | error messages |
| `--chosen` | `#fbead1` | `#37402a` | background of "Your choice" (peach mixed into cream) |

The dark theme (`prefers-color-scheme: dark`) uses the same hues on deep sage. It is an extension of the supplied palette,
not part of it. Remove the `@media (prefers-color-scheme: dark)` block in `web/style.css` if you want light only.

## Not controlled by the palette

The colours of the metro lines (Red, Yellow, Blue ...) come from `config/line_map.csv` so passengers recognise them. White
text on the Red Line is 4.49:1, a hair under the 4.5:1 target.

## Keeping it readable

A browser test renders the page in light and dark mode and measures the contrast of 69 text/background pairs (it fails if
any is below 4.5:1, or 3:1 for large text) and of control borders (3:1). Run it after any colour change.

## Name

MetroGo. The program files keep their old names (`metro_web.exe`, `metro_route.exe` ...); only what a person sees was renamed.
