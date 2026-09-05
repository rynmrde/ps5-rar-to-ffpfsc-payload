# MkPFS native compression benchmarks

The benchmark used identical sparse **256 MiB** folder fixtures on the Linux host and measured the complete native folder-to-`.ffpfsc` conversion. The output from each mode was byte-identical, and each image passed the upstream MkPFS verifier with **Warnings: 0** and **Errors: 0**.

| Mode | Time | Throughput | Relative result |
|---|---:|---:|---:|
| Serial (`1`) | 1.578249 s | 162.21 MiB/s | Baseline |
| Four workers (`4`) | 0.692410 s | 369.72 MiB/s | 2.28× faster |
| Auto | 0.622651 s | 411.15 MiB/s | 2.53× faster |

The worker pool is bounded and preserves deterministic PFSC block order: independent blocks are compressed in parallel, while the writer emits them in sequence. All three final-regression outputs were 813,428 bytes with SHA-256 `087bd1a7cf1c37fed8e638ffbe2fe5d1de1c36280c28281c0da3e19deb4fa185`. This is a host benchmark, not a PS5 performance claim and not a 50–100 GiB endurance result. The implementation uses bounded directory-entry buffers and 1 MiB file-data buffers, so memory behavior is intended to remain independent of total folder payload size. Large-volume measurements require a suitable storage target and a real PS5 or equivalent target environment.
