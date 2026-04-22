Run the clang static code analyzer on baresip-apps.

## Steps

1. Build and analyze with clang:

```bash
cmake -B build-clang \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-clang --target clean
scan-build --status-bugs -v cmake --build build-clang
```

Report any bugs or warnings found by the analyzer to the user.
