# ppmd - standalone PPMd compressor

```
ppmd 0.9.1
Usage: ppmd [OPTION]... [FILE]...

  -1...-9            set compression level (1 = fastest, 9 = best)
  -c, --stdout       write on standard output, keep original files unchanged
  -d, --decompress   decompress
  -f, --force        overwrite existing output files; allow terminal I/O
  -h, --help         give this help
  -k, --keep         keep (don't delete) input files
  -l, --list         list compressed file contents
  -L, --license      display software license
  -m, --memory=N     set memory in MB (default 16 MB; max 2048 H, 256 I)
  -n, --no-name      do not save or restore the original name and timestamp
  -N, --name         save or restore the original name and timestamp
  -o, --order=N      set order (default 6; max 32 H, 16 I)
  -q, --quiet        suppress warnings
  -t, --test         test compressed file integrity
  -T, --type=T       PPMd variant: h = PPMdH, i = PPMdI (default)
  -v, --verbose      verbose mode
  -V, --version      display version number
```

### Order & Memory Details

- **`-1` to `-9` (Presets):** Automatically configures order and memory:
  - **Variant I Order:** `3 + level` (4 to 12)
  - **Variant H Order:** `4, 4, 5, 5, 6, 8, 16, 24, 32`
  - **Memory (Both):** `2^(level - 1)` MB (1 MB to 256 MB)
- **`-o, --order=N`:** Context length (how many past bytes to analyze)
  - **Default:** 6 | **Max:** 32 (Variant H), 16 (Variant I)
- **`-m, --memory=N`:** Heap memory in MB (rounded down to a power of two)
  - **Default:** 16 MB | **Max:** 2048 MB (Variant H), 256 MB (Variant I)

# ppmd

`ppmd` is a lightweight data compression tool featuring a command-line interface similar to `gzip`/`xz`/`zstd`. It uses the **PPMd** (Prediction by Partial Matching, Variants H and I) compression algorithm. The algorithm specialises in high compression ratios on text files, logs, and source code.

The core compression and decompression routines utilize **Ppmd8** from the LZMA SDK.

## Credits & Acknowledgments
* **PPMd algorithms:** Developed by Dmitry Shkarin
* **LZMA SDK / Ppmd8 Implementation:** Adapted by Igor Pavlov (7-Zip project)

## License

This project is open source and available under the [The Unlicense](LICENSE) (Public Domain).
