# DebugIR

## How to use

Create a `debugir-abilist.txt` with e.g.:

```
fun:unoptimized_kprobe=debugir
```
to only add debug info for only that specific function.

## Misc
```
llvm-dwarfdump-12 --debug-line hello
objdump -d hello
cat ld-temp.debug-ll
```
