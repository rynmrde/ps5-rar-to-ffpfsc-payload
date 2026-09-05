# Benchmarks

The benchmark was run on the sandbox host using `tools/run_benchmark.sh` against a sparse 256 MiB folder fixture containing one zero-filled file. The native pipeline completed in **1.334770 seconds**, producing an 813,428-byte `.ffpfsc`. The upstream MkPFS verifier returned exit code 0 for the generated image and reported verification throughput of approximately 490.77 MiB/s.

This is a host smoke benchmark, not a PS5 benchmark and not a 50–100 GiB endurance result. The implementation uses bounded directory-entry buffers and 1 MiB file-data buffers, so its memory behavior is intended to remain independent of total folder payload size. A large-volume benchmark still requires a suitable storage target and a real PS5 or equivalent target environment.
