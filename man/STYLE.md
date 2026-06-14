# Man Page Style Guide

Write mdoc(7) format. Use `man/mbt_bitmap.3` as a reference.

## Structure

```
.Dd June 14, 2026
.Dt MBT_EXAMPLE 3
.Os
.Sh NAME
.Nm func1 ,
.Nm func2
.Nd one-line description
.Sh SYNOPSIS
.In mbt/example.h
.Ft return_type
.Fn func_name "arg1" "arg2"
.Sh DESCRIPTION
Prose here.
.Sh SEE ALSO
.Xr mbt 3
.Sh AUTHORS
.An Nick Owens Aq Mt mischief@offblast.org
```

## Directives

| Macro | Use |
|-------|-----|
| `.Dd` | Document date (top, once) |
| `.Dt` | Document title (top, once: `.Dt MBT_FOO 3`) |
| `.Os` | Operating system (top, once, no args) |
| `.Sh` | Section header (NAME, SYNOPSIS, DESCRIPTION, SEE ALSO, AUTHORS) |
| `.Nm` | Function name in NAME section (trailing comma except last) |
| `.Nd` | One-line description after NAME entries |
| `.In` | Include header (`.In mbt/foo.h`) |
| `.Ft` | Function return type (SYNOPSIS only, NOT `.Vt`) |
| `.Fn` | Function name + args (`.Fn name "type arg" "type arg"`) |
| `.Fa` | Function argument reference in prose |
| `.Pp` | Paragraph break (never use blank lines) |
| `.Vt` | Variable/type name in prose |
| `.Dv` | Defined value / constant |
| `.Xr` | Cross-reference (`.Xr mbt_session 3`) |
| `.An` | Author name |
| `.Aq` | Angle bracket enclosure |
| `.Mt` | Mail address |

## Rules

- NO blank lines — use `.Pp` for paragraph breaks
- NO `.Dt` anywhere except line 2 (document title)
- NO `.Fs`, `.Ff` — these don't exist
- `.Dt` is NOT a type reference — use `.Vt` for types in prose
- `.Bl` requires a list type: `.Bl -bullet`, `.Bl -tag -width 8`, `.Bl -item`
- Keep lines under 80 chars
- Descriptions: concise, factual, no filler
- One `.Ft` + `.Fn` pair per function in SYNOPSIS
- SEE ALSO: always include `.Xr mbt 3`
