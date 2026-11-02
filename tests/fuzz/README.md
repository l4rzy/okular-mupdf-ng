# IPC protocol fuzzing

Build with Clang, generate seeds, and fuzz each IPC target for one minute:

```sh
make fuzz
```

Override `FUZZ_ARGS` for a longer or repeatable run, for example
`make fuzz FUZZ_ARGS='-seed=1 -runs=10000'`. Set `FUZZ_BUILD_DIR` or `FUZZ_CXX`
to select another build directory or Clang compiler. The default build directory
retains the corpus and any crash artifacts between runs.

To run the individual steps manually:

```sh
cmake -S tests/fuzz -B build-ipc-fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ipc-fuzz
ctest --test-dir build-ipc-fuzz --output-on-failure
build-ipc-fuzz/ipc_fuzz_seeds build-ipc-fuzz/corpus-codec
build-ipc-fuzz/test_ipc_fuzz_framing build-ipc-fuzz/corpus-framing
build-ipc-fuzz/fuzz_ipc_codec build-ipc-fuzz/corpus-codec -max_len=65536 -rss_limit_mb=2048 -max_total_time=60
build-ipc-fuzz/fuzz_ipc_framing build-ipc-fuzz/corpus-framing -max_len=65536 -rss_limit_mb=2048 -max_total_time=60
```

The standalone build instruments the codec and transport dependencies with
libFuzzer coverage, AddressSanitizer, and UndefinedBehaviorSanitizer. The harness
checks all three message families, failed-decode destination preservation, and
decode/encode stability. The seed generator validates its output and covers every
payload variant, plus populated strings, containers, optional fields, and error
messages. The fuzzer adds inputs to the corpus directory; retain it between runs.
Regenerate seeds after protocol changes in a fresh directory, then merge useful
inputs from an older corpus with libFuzzer's `-merge=1` option.

The framing target sends raw stream bytes in variable-sized chunks through a
nonblocking Unix socket pair. It compares each read and payload with a framing
model, including partial frames, multiple frames, invalid lengths, and peer
closure, including closure while data is still queued. Seeds include a sequence
of populated frames followed by a truncated frame, exercising persistent parser
state across many reads. Its first input byte controls fragmentation and whether
to close with data queued; the rest is wire data.
The harness accepts inputs up to 65537 bytes. Socket creation must be permitted
by the environment.

For a repeatable short run, add `-seed=1 -runs=10000`. Reproduce a crash by
passing its saved file directly to the fuzzer, and minimize it with
`-minimize_crash=1` before adding a regression test. The sample 64 KiB input
limit favors throughput and does not cover complete frames at the 64 MiB protocol
limit. Descriptor passing is not fuzzed by these targets.
