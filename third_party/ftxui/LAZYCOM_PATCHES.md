# LazyCom vendored-tree trimming

The vendored FTXUI 6.1.9 tree contains Linux production build inputs. Upstream
tests, examples and documentation are not part of the LazyCom build.

Test, fuzzing and benchmark CMake modules and `component_fuzzer.cpp` are kept in
the separate `lazycom-test` repository. Their three includes were removed from
this tree's `CMakeLists.txt`. Production implementation files are unchanged.
The dependency lock still identifies the original upstream archive.
