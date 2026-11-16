#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include "Ppmd7.h"
#include "Ppmd8.h"

#define IS_TTY(f)     isatty(fileno(f))

#define FLAG_MTIME       0x8000  // fnlen bit 15: an 8-byte mtime follows the name (-N)
#define IO_BUFFER_SIZE   65536
#define MAGIC            0x84ACAF8Fu
#define MAX_NAME_LEN     0x1FF   // bits 0-8 of the header's fnlen field
#define PPMD_BUFFER_SIZE 65536
#define PPMD_EXT         ".ppmd"
#define PPMD_LICENSE     "This is free and unencumbered software released into the public domain."
#define PPMD_VERSION     "0.9.1"

static void *pmalloc(ISzAllocPtr ip, size_t size) { (void) ip; return malloc(size); }
static void pfree(ISzAllocPtr ip, void *addr)     { (void) ip; free(addr); }
static ISzAlloc ialloc = { pmalloc, pfree };

struct CharWriter {
    void (*Write)(void *p, Byte b);
    FILE *fp;
};

struct CharReader {
    Byte (*Read)(void *p);
    FILE *fp;
    bool eof;
};

static void Write(void *p, Byte b)
{
    putc_unlocked(b, ((struct CharWriter *)p)->fp);
}

static Byte Read(void *p)
{
    struct CharReader *cr = p;
    int c = cr->eof ? EOF : getc_unlocked(cr->fp);
    if (c == EOF) {
        cr->eof = true;
        return 0;
    }
    return (Byte)c;
}

// magic(4) + info(2) + fnlen(2), then the base file name (fnlen & 0x1FF bytes),
// then, if fnlen & 0x8000, the original mtime (8 bytes, signed seconds)
// bits 0-4 order-1, bits 5-9 log2(mem bytes)-1, bits 12-15 variant ('h'/'i' - 'a').
// Not a 7-Zip container; only the PPMd model code is shared with 7-Zip

static void put_le(unsigned char *p, unsigned long long v, int n)
{
    for (int i = 0; i < n; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}

static unsigned long long get_le(const unsigned char *p, int n)
{
    unsigned long long v = 0;
    for (int i = 0; i < n; ++i)
        v |= (unsigned long long)p[i] << (8 * i);
    return v;
}

static const char *progname = "ppmd";
static const char *volatile partial_output = NULL;

static struct {
    bool decompress, tostdout, keep, force, verbose, quiet, noname, name, test, list;
    int level;    // -1..-9, 0 = not set
    int order;    // -o, 0 = not set (derived after parsing)
    int mem_mb;   // -m, 0 = not set (derived after parsing)
    int mem;      // resolved model memory, log2 of bytes
    char variant; // 'h' = PPMdH (Ppmd7), 'i' = PPMdI (Ppmd8)
} opt = { .variant = 'i' };

static void vmsg(const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", progname);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static void errmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(fmt, ap);
    va_end(ap);
}

static void warnmsg(const char *fmt, ...)
{
    if (opt.quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vmsg(fmt, ap);
    va_end(ap);
}

static void on_signal(int sig)
{
    if (partial_output) unlink(partial_output);
    signal(sig, SIG_DFL);
    raise(sig);
}

// per-variant limits
static int max_order(char variant)   { return variant == 'h' ? 32 : PPMD8_MAX_ORDER; }
static int max_mem_exp(char variant) { return variant == 'h' ? 31 : 28; }

// 7-Zip maps level to order per variant: Ppmd7 (.7z) uses a fixed table,
// Ppmd8 (.zip) uses 3+level. Memory is 2^(level-1) MB for both
static int level_order(int lvl, char variant)
{
    static const unsigned char orders_h[10] = { 3, 4, 4, 5, 5, 6, 8, 16, 24, 32 };
    return variant == 'h' ? orders_h[lvl] : 3 + lvl;
}

static int floor_log2(int v)
{
    int lg = 0;
    while (v >> (lg + 1)) ++lg;
    return lg;
}

static bool encode_h(FILE *in, FILE *out, int order, int mem)
{
    struct CharWriter cw = { Write, out };
    CPpmd7 ppmd;
    Ppmd7_Construct(&ppmd);
    ppmd.rc.enc.Stream = (IByteOut *) &cw;

    if (!Ppmd7_Alloc(&ppmd, (UInt32)1 << mem, &ialloc)) {
        errmsg("out of memory for PPMd allocation");
        return false;
    }
    Ppmd7z_Init_RangeEnc(&ppmd);
    Ppmd7_Init(&ppmd, (unsigned)order);

    unsigned char buf[PPMD_BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        Ppmd7z_EncodeSymbols(&ppmd, buf, buf + n);

    bool ok = !ferror(in);
    if (ok) {
        Ppmd7z_EncodeSymbol(&ppmd, PPMD7_SYM_END);
        Ppmd7z_Flush_RangeEnc(&ppmd);
    } else {
        errmsg("read error");
    }
    Ppmd7_Free(&ppmd, &ialloc);
    return ok;
}

static bool encode_i(FILE *in, FILE *out, int order, int mem)
{
    struct CharWriter cw = { Write, out };
    CPpmd8 ppmd;
    Ppmd8_Construct(&ppmd);
    ppmd.Stream.Out = (IByteOut *) &cw;

    if (!Ppmd8_Alloc(&ppmd, (UInt32)1 << mem, &ialloc)) {
        errmsg("out of memory for PPMd allocation");
        return false;
    }
    Ppmd8_Init_RangeEnc(&ppmd);
    Ppmd8_Init(&ppmd, (unsigned)order, PPMD8_RESTORE_METHOD_RESTART);

    unsigned char buf[PPMD_BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        for (size_t i = 0; i < n; ++i)
            Ppmd8_EncodeSymbol(&ppmd, buf[i]);

    bool ok = !ferror(in);
    if (ok) {
        Ppmd8_EncodeSymbol(&ppmd, PPMD8_SYM_END);
        Ppmd8_Flush_RangeEnc(&ppmd);
    } else {
        errmsg("read error");
    }
    Ppmd8_Free(&ppmd, &ialloc);
    return ok;
}

// Output errors are detected by the caller (ferror/fclose on output stream)
static bool decode_verdict(bool end_ok, const struct CharReader *cr)
{
    if (end_ok) {
        if (getc_unlocked(cr->fp) != EOF)
            warnmsg("trailing garbage after compressed stream ignored");
        return true;
    }
    if (ferror(cr->fp))  errmsg("read error");
    else if (cr->eof)    errmsg("unexpected end of compressed data");
    else                 errmsg("compressed data is corrupt");
    return false;
}

static bool decode_h(FILE *in, FILE *out, int order, int mem, unsigned long long *size)
{
    struct CharReader cr = { Read, in, false };
    CPpmd7 ppmd;
    Ppmd7_Construct(&ppmd);
    ppmd.rc.dec.Stream = (IByteIn *) &cr;

    if (!Ppmd7_Alloc(&ppmd, (UInt32)1 << mem, &ialloc)) {
        errmsg("out of memory for PPMd allocation");
        return false;
    }
    Ppmd7z_RangeDec_Init(&ppmd.rc.dec);
    Ppmd7_Init(&ppmd, (unsigned)order);

    int c;
    unsigned long long n = 0;
    while ((c = Ppmd7z_DecodeSymbol(&ppmd)) >= 0 && !cr.eof) {
        ++n;
        if (out) putc_unlocked(c, out); // out == NULL: -t / -l, decode only
    }
    *size = n;

    bool ok = decode_verdict(c == PPMD7_SYM_END && !cr.eof && Ppmd7z_RangeDec_IsFinishedOK(&ppmd.rc.dec), &cr);
    Ppmd7_Free(&ppmd, &ialloc);
    return ok;
}

static bool decode_i(FILE *in, FILE *out, int order, int mem, unsigned long long *size)
{
    struct CharReader cr = { Read, in, false };
    CPpmd8 ppmd;
    Ppmd8_Construct(&ppmd);
    ppmd.Stream.In = (IByteIn *) &cr;

    if (!Ppmd8_Alloc(&ppmd, (UInt32)1 << mem, &ialloc)) {
        errmsg("out of memory for PPMd allocation");
        return false;
    }
    Ppmd8_Init_RangeDec(&ppmd);
    Ppmd8_Init(&ppmd, (unsigned)order, PPMD8_RESTORE_METHOD_RESTART);

    int c;
    unsigned long long n = 0;
    while ((c = Ppmd8_DecodeSymbol(&ppmd)) >= 0 && !cr.eof) {
        ++n;
        if (out) putc_unlocked(c, out);
    }
    *size = n;

    bool ok = decode_verdict(c == PPMD8_SYM_END && !cr.eof &&
                             Ppmd8_RangeDec_IsFinishedOK(&ppmd), &cr);
    Ppmd8_Free(&ppmd, &ialloc);
    return ok;
}

static bool compress_stream(FILE *in, FILE *out, const char *fname, int order, int mem, const struct stat *st)
{
    const char *slash = strrchr(fname, '/');
    const char *base = slash ? slash + 1 : fname;
    size_t len = opt.noname ? 0 : strlen(base); // -n: store no name (fnlen = 0)
    bool stamp = opt.name && st;                // -N: also store the mtime (files only)

    unsigned char hdr[8], mtime[8];
    put_le(hdr, MAGIC, 4);
    put_le(hdr + 4, (unsigned)((order - 1) | ((mem - 1) << 5) | ((opt.variant - 'a') << 12)), 2);
    put_le(hdr + 6, len | (stamp ? FLAG_MTIME : 0), 2);
    put_le(mtime, (unsigned long long)(long long)(stamp ? st->st_mtime : 0), 8);

    if (fwrite(hdr, 1, sizeof hdr, out) != sizeof hdr ||
        (len && fwrite(base, 1, len, out) != len) ||
        (stamp && fwrite(mtime, 1, sizeof mtime, out) != sizeof mtime))
        return false;

    return opt.variant == 'i' ? encode_i(in, out, order, mem) : encode_h(in, out, order, mem);
}

struct Header {
    char variant;
    int order, mem;
    bool has_mtime;
    time_t mtime;
    char name[MAX_NAME_LEN + 1]; // "" if none
};

static bool read_header(FILE *in, struct Header *h)
{
    unsigned char hdr[8];
    size_t got = fread(hdr, 1, sizeof hdr, in);
    if (got == 0)            { errmsg("empty input"); return false; }
    if (got != sizeof hdr)   { errmsg("truncated or invalid file header"); return false; }
    if (get_le(hdr, 4) != MAGIC) { errmsg("not a valid ppmd file (bad magic)"); return false; }

    unsigned info = (unsigned)get_le(hdr + 4, 2);
    unsigned fn = (unsigned)get_le(hdr + 6, 2);
    h->variant = (char)('a' + (info >> 12));
    if (h->variant != 'h' && h->variant != 'i') { errmsg("invalid header format signature"); return false; }
    if (fn & ~(unsigned)(MAX_NAME_LEN | FLAG_MTIME)) { errmsg("unsupported header flags"); return false; }

    size_t nlen = fn & MAX_NAME_LEN;
    if (fread(h->name, 1, nlen, in) != nlen) { errmsg("truncated filename field"); return false; }
    h->name[nlen] = '\0';
    if (memchr(h->name, '\0', nlen)) h->name[0] = '\0'; // embedded NUL: ignore the name

    h->has_mtime = fn & FLAG_MTIME;
    if (h->has_mtime) {
        unsigned char t[8];
        if (fread(t, 1, sizeof t, in) != sizeof t) { errmsg("truncated timestamp field"); return false; }
        h->mtime = (time_t)(long long)get_le(t, 8);
    }

    h->order = (int)(info & 0x1f) + 1;
    h->mem = (int)((info >> 5) & 0x1f) + 1;
    if (h->order < 2 || h->order > max_order(h->variant) || h->mem < 11 || h->mem > max_mem_exp(h->variant)) {
        errmsg("invalid model parameters in header");
        return false;
    }
    return true;
}

// out == NULL: decode only (-t / -l). *size = number of bytes decoded.
static bool decode(FILE *in, FILE *out, const struct Header *h, unsigned long long *size)
{
    return h->variant == 'i' ? decode_i(in, out, h->order, h->mem, size)
                             : decode_h(in, out, h->order, h->mem, size);
}

static bool has_ppmd_ext(const char *s)
{
    size_t ls = strlen(s), lx = strlen(PPMD_EXT);
    return ls >= lx && strcmp(s + ls - lx, PPMD_EXT) == 0;
}

// Returns 0 and sets *outname, or 1 (error) / 2 (file ignored)
static int make_outname(const char *name, char **outname)
{
    size_t ln = strlen(name), lx = strlen(PPMD_EXT);
    bool ext = has_ppmd_ext(name);

    if (opt.decompress && (!ext || ln == lx)) {
        warnmsg("%s: unknown suffix -- ignored", name);
        return 2;
    }
    if (!opt.decompress && ext) {
        warnmsg("%s already has %s suffix -- unchanged", name, PPMD_EXT);
        return 2;
    }

    size_t keep = opt.decompress ? ln - lx : ln;
    const char *suffix = opt.decompress ? "" : PPMD_EXT;
    size_t size = keep + strlen(suffix) + 1;
    char *p = malloc(size);
    if (!p) { errmsg("out of memory"); return 1; }
    snprintf(p, size, "%.*s%s", (int)keep, name, suffix);
    *outname = p;
    return 0;
}

// -N when decompressing: output name = stored name, placed next to the input file.
// Returns NULL if the header has no usable name.
static char *stored_outname(const char *inname, const struct Header *h)
{
    const char *n = h->name, *s = strrchr(n, '/');
    if (s) n = s + 1; // never trust directories from the header
    if (!*n || strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return NULL;

    const char *slash = strrchr(inname, '/');
    int dirlen = slash ? (int)(slash - inname) + 1 : 0;
    size_t size = (size_t)dirlen + strlen(n) + 1;
    char *p = malloc(size);
    if (p) snprintf(p, size, "%.*s%s", dirlen, inname, n);
    return p;
}

static bool may_overwrite(const char *path)
{
    struct stat st;
    if (opt.force || stat(path, &st) != 0) return true;

    bool yes = false;
    if (IS_TTY(stdin) && IS_TTY(stderr)) {
        fprintf(stderr, "%s: %s already exists; do you wish to overwrite (y or n)? ", progname, path);
        int ch = getchar(), skip = ch;
        while (skip != '\n' && skip != EOF) skip = getchar();
        yes = (ch == 'y' || ch == 'Y');
    }
    if (!yes) warnmsg("%s already exists -- not overwritten", path);
    return yes;
}

static unsigned long long list_total_c, list_total_u;
static int list_count;

static double list_ratio(unsigned long long c, unsigned long long u)
{
    return u ? 100.0 * ((double)u - (double)c) / (double)u : 0.0;
}

static void list_row(const char *name, const struct Header *h, long long csize, unsigned long long usize)
{
    if (list_count++ == 0)
        printf("%19s %19s %6s %s\n", "compressed", "uncompressed", "ratio", "uncompressed_name");

    const char *shown = h->name;
    int len = (int)strlen(shown);
    if (!len) {
        shown = name ? name : "-";
        len = (int)strlen(shown);
        if (name && has_ppmd_ext(name) && len > (int)strlen(PPMD_EXT)) len -= (int)strlen(PPMD_EXT);
    }

    char c[24] = "-", ratio[16] = "-";
    if (csize >= 0) {
        snprintf(c, sizeof c, "%lld", csize);
        snprintf(ratio, sizeof ratio, "%.1f%%", list_ratio((unsigned long long)csize, usize));
        list_total_c += (unsigned long long)csize;
    }
    list_total_u += usize;
    printf("%19s %19llu %6s %.*s\n", c, usize, ratio, len, shown);
}

static void list_totals(void)
{
    if (list_count > 1)
        printf("%19llu %19llu %5.1f%% (totals)\n", list_total_c, list_total_u,
               list_ratio(list_total_c, list_total_u));
}

static int bail(FILE *in, char *outname, int rc)
{
    if (in != stdin) fclose(in);
    free(outname);
    return rc;
}

static int do_file(const char *name)
{
    const bool discard = opt.test || opt.list;
    const bool to_std = opt.tostdout || discard || !name;
    FILE *in = stdin, *out = discard ? NULL : stdout;
    char *outname = NULL;
    struct stat st;
    struct Header hdr;
    int rc;

    memset(&st, 0, sizeof st);
    memset(&hdr, 0, sizeof hdr);
    if (name) {
        const char *slash = strrchr(name, '/');
        const char *base = slash ? slash + 1 : name;
        if (strlen(base) > MAX_NAME_LEN) { errmsg("filename too long (max %d bytes): %s", MAX_NAME_LEN, base); return 1; }
        if (stat(name, &st) != 0) { errmsg("%s: %s", name, strerror(errno)); return 1; }
        if (S_ISDIR(st.st_mode)) { warnmsg("%s is a directory -- ignored", name); return 2; }
        if (!S_ISREG(st.st_mode) && !opt.tostdout && !discard) { warnmsg("%s is not a regular file -- ignored", name); return 2; }
        if (!to_std && (rc = make_outname(name, &outname)) != 0) return rc;
    }

    if (!name && opt.decompress && IS_TTY(stdin) && !opt.force) {
        errmsg("compressed data not read from a terminal. Use -f to force decompression.");
        return 1;
    }
    if (to_std && !opt.decompress && IS_TTY(stdout) && !opt.force) {
        errmsg("compressed data not written to a terminal. Use -f to force compression.");
        return 1;
    }

    if (name) {
        if (!(in = fopen(name, "rb"))) { errmsg("%s: %s", name, strerror(errno)); return bail(stdin, outname, 1); }
        setvbuf(in, NULL, _IOFBF, IO_BUFFER_SIZE);
    }

    if (opt.decompress && !read_header(in, &hdr)) return bail(in, outname, 1);

    if (outname && opt.decompress && opt.name) {
        char *stored = stored_outname(name, &hdr);
        if (stored && strcmp(stored, name) != 0) { free(outname); outname = stored; }
        else free(stored);
    }

    if (outname) {
        if (!may_overwrite(outname)) return bail(in, outname, 2);
        if (!(out = fopen(outname, "wb"))) {
            errmsg("%s: %s", outname, strerror(errno));
            return bail(in, outname, 1);
        }
        setvbuf(out, NULL, _IOFBF, IO_BUFFER_SIZE);
        partial_output = outname;
    }

    unsigned long long usize = 0;
    bool ok = opt.decompress ? decode(in, out, &hdr, &usize)
                             : compress_stream(in, out, name ? name : "stdin", opt.order, opt.mem, name ? &st : NULL);

    if (out) {
        bool werr = fflush(out) != 0 || ferror(out);
        if (out != stdout && fclose(out) != 0) werr = true;
        if (werr) { errmsg("%s: write error: %s", outname ? outname : "stdout", strerror(errno)); ok = false; }
    }

    long long csize = -1; // compressed size for -l, if the input is a regular file
    if (opt.list) {
        struct stat ist;
        if (fstat(fileno(in), &ist) == 0 && S_ISREG(ist.st_mode)) csize = (long long)ist.st_size;
    }
    if (in != stdin) fclose(in);

    if (!ok) {
        if (outname) remove(outname);
        partial_output = NULL;
        free(outname);
        return 1;
    }

    if (opt.list) {
        list_row(name, &hdr, csize, usize);
    } else if (opt.verbose) {
        fprintf(stderr, "%s:\t", name ? name : "stdin");
        if (opt.test) fputs(" OK", stderr);
        else if (outname) fprintf(stderr, " -- %s %s", opt.keep ? "created" : "replaced with", outname);
        fputc('\n', stderr);
    }

    if (outname) {
        if (chmod(outname, st.st_mode & 07777) != 0)
            warnmsg("%s: cannot set mode: %s", outname, strerror(errno));
        // -n: do not restore the original timestamp
        // default: timestamp of the input file; -N: the one stored in header
        if (!opt.noname) {
            time_t mtime = (opt.decompress && opt.name && hdr.has_mtime) ? hdr.mtime : st.st_mtime;
            struct utimbuf ub = { st.st_atime, mtime };
            if (utime(outname, &ub) != 0)
                warnmsg("%s: cannot set times: %s", outname, strerror(errno));
        }
        partial_output = NULL;
        if (!opt.keep && remove(name) != 0)
            warnmsg("%s: cannot remove: %s", name, strerror(errno));
        free(outname);
    }
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "%s %s\n"
        "Usage: %s [OPTION]... [FILE]...\n"
        "\n"
        "  -1...-9            set compression level (1 = fastest, 9 = best)\n"
        "  -c, --stdout       write on standard output, keep original files unchanged\n"
        "  -d, --decompress   decompress\n"
        "  -f, --force        overwrite existing output files; allow terminal I/O\n"
        "  -h, --help         give this help\n"
        "  -k, --keep         keep (don't delete) input files\n"
        "  -l, --list         list compressed file contents\n"
        "  -L, --license      display software license\n"
        "  -m, --memory=N     set memory in MB (default 16 MB; max 2048 H, 256 I)\n"
        "  -n, --no-name      do not save or restore the original name and timestamp\n"
        "  -N, --name         save or restore the original name and timestamp\n"
        "  -o, --order=N      set order (default 6; max 32 H, 16 I)\n"
        "  -q, --quiet        suppress warnings\n"
        "  -t, --test         test compressed file integrity\n"
        "  -T, --type=T       PPMd variant: h = PPMdH, i = PPMdI (default)\n"
        "  -v, --verbose      verbose mode\n"
        "  -V, --version      display version number\n",
        progname, PPMD_VERSION, progname);
}

// strict integer parse: whole string must be a number in [lo, hi]
static bool parse_int(const char *s, long lo, long hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

int main(int argc, char **argv)
{
    const char *slash = strrchr(argv[0], '/');
    const char *pn = slash ? slash + 1 : argv[0];
    if (*pn) progname = pn;
    if (strncmp(progname, "un", 2) == 0) opt.decompress = true;
    if (strstr(progname, "cat")) { opt.decompress = true; opt.tostdout = true; }

    setvbuf(stdin, NULL, _IOFBF, IO_BUFFER_SIZE);
    setvbuf(stdout, NULL, _IOFBF, IO_BUFFER_SIZE);

    static const struct option longopts[] = {
        { "stdout",     0, NULL, 'c' },
        { "decompress", 0, NULL, 'd' },
        { "uncompress", 0, NULL, 'd' },
        { "keep",       0, NULL, 'k' },
        { "force",      0, NULL, 'f' },
        { "list",       0, NULL, 'l' },
        { "license",    0, NULL, 'L' },
        { "no-name",    0, NULL, 'n' },
        { "name",       0, NULL, 'N' },
        { "memory",     1, NULL, 'm' },
        { "order",      1, NULL, 'o' },
        { "test",       0, NULL, 't' },
        { "type",       1, NULL, 'T' },
        { "verbose",    0, NULL, 'v' },
        { "quiet",      0, NULL, 'q' },
        { "help",       0, NULL, 'h' },
        { "version",    0, NULL, 'V' },
        {  NULL,        0, NULL,  0  }
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "cdfhklLm:nNo:qtT:vV123456789", longopts, NULL)) != -1) {
        switch (ch) {
            case 'c': opt.tostdout = true; break;
            case 'd': opt.decompress = true; break;
            case 'k': opt.keep = true; break;
            case 'f': opt.force = true; break;
            case 'l': opt.decompress = opt.list = true; break;
            case 'L': printf("%s %s\n%s\n\nThe PPMd model code (lib/Ppmd7*, lib/Ppmd8*) is by Dmitry Shkarin and\n"
                             "Igor Pavlov (7-Zip), released into the public domain.\n",
                             progname, PPMD_VERSION, PPMD_LICENSE);
                      return 0;
            case 'n': opt.noname = true; opt.name = false; break;
            case 'N': opt.name = true; opt.noname = false; break;
            case 't': opt.decompress = opt.test = true; break;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                opt.level = ch - '0';
                opt.order = opt.mem_mb = 0;
                break;
            case 'm':
                if (!parse_int(optarg, 1, INT_MAX, &opt.mem_mb)) { errmsg("invalid memory %s MB (use >= 1)", optarg); return 1; }
                break;
            case 'o':
                if (!parse_int(optarg, 2, 32, &opt.order)) { errmsg("invalid order %s (use 2-32)", optarg); return 1; }
                break;
            case 'T': {
                char v = (char)tolower((unsigned char)optarg[0]);
                if (optarg[1] != '\0' || (v != 'h' && v != 'i')) { errmsg("invalid variant %s (use h or i)", optarg); return 1; }
                opt.variant = v;
                break;
            }
            case 'v': opt.verbose = true; break;
            case 'q': opt.quiet = true; break;
            case 'h': usage(stdout); return 0;
            case 'V': printf("%s %s\n", progname, PPMD_VERSION); return 0;
            default:  usage(stderr); return 1;
        }
    }

    if (!opt.decompress) {
        if (!opt.order)
            opt.order = opt.level ? level_order(opt.level, opt.variant) : 6;
        if (opt.order > max_order(opt.variant)) {
            errmsg("order %d too high for variant %c (max %d)", opt.order, opt.variant, max_order(opt.variant));
            return 1;
        }
        opt.mem = opt.mem_mb ? 20 + floor_log2(opt.mem_mb)  // MB rounded down to 2^k
                : opt.level  ? 19 + opt.level               // 2^(level-1) MB
                :              24;                          // 16 MB default
        if (opt.mem_mb > 1 << (max_mem_exp(opt.variant) - 20)) {
            errmsg("invalid memory %d MB for variant %c (max %d MB)", opt.mem_mb, opt.variant,
                   1 << (max_mem_exp(opt.variant) - 20));
            return 1;
        }
    }

    const int nfiles = argc - optind;
    if (!opt.decompress && opt.tostdout && nfiles > 1) {
        errmsg("cannot compress multiple files to stdout");
        return 1;
    }

    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; ++i)
        if (signal(sigs[i], SIG_IGN) != SIG_IGN) signal(sigs[i], on_signal);

    int status = 0;
    for (int i = 0; i < (nfiles ? nfiles : 1); ++i) {
        const char *f = nfiles ? argv[optind + i] : NULL;
        int r = do_file(f && strcmp(f, "-") != 0 ? f : NULL);
        if (r == 1) status = 1;
        else if (r == 2 && status == 0) status = 2;
    }
    if (opt.list) list_totals();
    return status;
}
