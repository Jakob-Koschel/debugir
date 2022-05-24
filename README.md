# DebugIR

```
llvm-project/build/bin/clang -flto -c -o hello.o hello.c
llvm-project/build/bin/opt -debugir hello.o -o hello.debugir.o
clang-12 -flegacy-pass-manager -fuse-ld=lld -flto=full hello.debugir.o -o hello

llvm-project/build/bin/clang -flegacy-pass-manager -fuse-ld=lld -flto=full -o hello hello.c
llvm-dwarfdump-12 --debug-line hello
objdump -d hello
cat ld-temp.debug-ll
```
