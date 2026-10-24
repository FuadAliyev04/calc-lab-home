// Fuad Aliyev 251ADB190
// (optional) https://github.com/FuadAliyev04/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic AI assistant), conversation history: <PASTE SHARE LINK HERE>.
//   Details in comments where I used it.

// mkdir(), opendir(), stat() and realpath() are POSIX, not plain C17. This macro makes them
// visible even though we compile with -std=c17.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Used in output names: <input_base>_<name>_<lastname>_<studentid>.txt
#define FIRST "fuad"
#define LAST "aliyev"
#define SID "251ADB190"

// ---------------------------------------------------------------------------------------
// Overall design
// AI (Claude): prompt "bu benim en sonuncu labim bunu nasil yazmam gerkeiyro" (I pasted the
//   lab text). Claude suggested: read the whole file into memory, recursive descent with one
//   function per precedence level, and one global error flag. The code below is based on
//   that plan, and I went through each function to be able to explain it.
//
// Positions: the whole file lives in ONE array (src), so the position of any character is
// simply its index + 1. That automatically counts every '\n' and every character of a comment
// line, which is exactly what the position rules ask for. Reading line by line (fgets) would
// lose that, and would also be hard on very long lines.
// ---------------------------------------------------------------------------------------
static char *src;       // the whole input file, with src[len] == '\0' as a safe sentinel
static size_t len;      // number of characters in src
static size_t pos;      // index of the character the parser is looking at
static size_t err_pos;  // 0 = no error yet, otherwise the 1-based position of the first error

// A number is either an integer or a double. is_float decides which field is valid.
typedef struct {
    int is_float;
    long long i;
    double d;
} Value;

// Growable output buffer. Results are collected here and written at the end, so that an
// error can throw away every partial result ("no partial results" rule).
typedef struct {
    char *data;
    size_t len, cap;
} Buf;

// Reads a whole file into memory. The capacity doubles when the buffer is almost full, so each
// byte is copied only a constant number of times: reading a multi-megabyte file stays linear.
// AI (Claude): prompt "how do I read a whole file into memory in C without knowing its size".
//   I used the doubling idea; the loop details are my own.
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    if (!b) {
        fclose(f);
        return NULL;
    }
    size_t got;
    // cap - n - 1 keeps one byte free for the final '\0'
    while ((got = fread(b + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (cap - n < 2) {  // almost full: double the capacity
            cap *= 2;
            char *t = realloc(b, cap);
            if (!t) {
                free(b);
                fclose(f);
                return NULL;
            }
            b = t;
        }
    }
    if (ferror(f)) {  // for example when the path is a folder
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    b[n] = '\0';
    *out_len = n;
    return b;
}

// Appends n bytes to the buffer. Doubling the capacity avoids copying the whole output again
// for every result line.
static int buf_append(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *t = realloc(b->data, nc);
        if (!t) return -1;
        b->data = t;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

// Records an error at index (0-based). Only the FIRST error counts: the functions that unwind
// after an error must not overwrite it.
static void fail(size_t index) {
    if (err_pos == 0) err_pos = index + 1;
}

// Spaces and tabs may appear anywhere between tokens, so every token reader skips them first.
static void skip_ws(void) {
    while (pos < len && (src[pos] == ' ' || src[pos] == '\t')) pos++;
}

static double as_double(Value v) { return v.is_float ? v.d : (double)v.i; }

static int is_zero(Value v) { return v.is_float ? v.d == 0.0 : v.i == 0; }

// Integer power by repeated squaring: O(log e) steps, so a huge exponent such as 1 ** 10^12 is
// still fast. Unsigned arithmetic wraps around instead of being undefined behaviour on overflow.
static long long int_pow(long long b, long long e) {
    unsigned long long result = 1, base = (unsigned long long)b;
    while (e > 0) {
        if (e & 1) result *= base;
        e >>= 1;
        if (e) base *= base;
    }
    return (long long)result;
}

// Integer or float arithmetic for + - * /.
// Integer or float? If EITHER operand is a float the whole operation is done in double,
// otherwise in long long. So 7 / 2 is 3 but 7 / 2.0 is 3.5.
// A float result that is infinite or not a number is reported at the operator (op_pos).
// Division by zero is checked by the caller, because it is reported at the right operand.
static Value arith(char op, Value l, Value r, size_t op_pos) {
    Value v = {0, 0, 0.0};
    if (l.is_float || r.is_float) {
        double a = as_double(l), b = as_double(r), x;
        switch (op) {
            case '+': x = a + b; break;
            case '-': x = a - b; break;
            case '*': x = a * b; break;
            default: x = a / b; break;
        }
        v.is_float = 1;
        v.d = x;
        if (!isfinite(x)) fail(op_pos);
    } else {
        // Unsigned arithmetic: no signed-overflow undefined behaviour.
        unsigned long long a = (unsigned long long)l.i, b = (unsigned long long)r.i;
        switch (op) {
            case '+': v.i = (long long)(a + b); break;
            case '-': v.i = (long long)(a - b); break;
            case '*': v.i = (long long)(a * b); break;
            // C division truncates toward zero, as the rules want. The -1 case avoids the
            // one overflowing division (LLONG_MIN / -1).
            default: v.i = (r.i == -1) ? (long long)(0ULL - a) : l.i / r.i; break;
        }
    }
    return v;
}

// base ** exp. int ** int needs a non-negative exponent and gives an integer.
// With a float on either side it uses pow() in double, so 0.5 ** (0 - 1) is 2.
// A negative integer exponent is reported at the start of the exponent (exp_pos),
// an infinite or NaN float result at the operator (op_pos), e.g. (0 - 8) ** 0.5.
static Value power(Value b, Value e, size_t op_pos, size_t exp_pos) {
    Value v = {0, 0, 0.0};
    if (b.is_float || e.is_float) {
        v.is_float = 1;
        v.d = pow(as_double(b), as_double(e));
        if (!isfinite(v.d)) fail(op_pos);
    } else if (e.i < 0) {
        fail(exp_pos);
    } else {
        v.i = int_pow(b.i, e.i);
    }
    return v;
}

static Value parse_expr(void);

// primary := NUMBER | '(' expr ')'
// Every error here is reported where the parser stands, because this is the start of the
// token it could not use: a bad character, a second operator in a row, an unwanted ')' as in
// "2 * ()", or the '\n' (or end of file) when an operand is missing at the end of a line.
// Because there is no rule for a leading '-', "-5" and "2 * -3" end up here as well and are
// reported at the '-'.
static Value parse_primary(void) {
    Value v = {0, 0, 0.0};
    skip_ws();
    if (pos < len && isdigit((unsigned char)src[pos])) {
        size_t start = pos;
        unsigned long long acc = 0;
        while (pos < len && isdigit((unsigned char)src[pos])) {
            acc = acc * 10 + (unsigned long long)(src[pos] - '0');
            pos++;
        }
        // A float needs digits on BOTH sides of the '.'. For "5." or ".5" nothing special
        // happens here: the '.' is simply left over and the caller reports it (at the '.').
        if (src[pos] == '.' && isdigit((unsigned char)src[pos + 1])) {
            pos++;
            while (pos < len && isdigit((unsigned char)src[pos])) pos++;
            // strtod needs a '\0' after the number, so end the string temporarily instead of
            // copying the digits somewhere else.
            char saved = src[pos];
            src[pos] = '\0';
            v.is_float = 1;
            v.d = strtod(src + start, NULL);
            src[pos] = saved;
            if (!isfinite(v.d)) fail(start);
        } else {
            v.i = (long long)acc;
        }
    } else if (pos < len && src[pos] == '(') {
        pos++;
        v = parse_expr();
        if (err_pos) return v;
        skip_ws();
        // Missing ')' at the end of a line is reported at the '\n' (or file length + 1),
        // the place where the ')' should have been.
        if (pos < len && src[pos] == ')') pos++;
        else fail(pos);
    } else {
        fail(pos);
    }
    return v;
}

// power := primary [ '**' power ]
// The right operand calls power() again, not primary(), so 2 ** 3 ** 2 groups as 2 ** (3 ** 2).
// That is what makes ** right-associative. It also binds tighter than * and /, because
// term() only ever sees complete power() results.
static Value parse_power(void) {
    Value base = parse_primary();
    if (err_pos) return base;
    skip_ws();
    if (src[pos] == '*' && src[pos + 1] == '*') {  // src[len] == '\0', so this is safe
        size_t op = pos;
        pos += 2;
        skip_ws();
        size_t exp_start = pos;
        Value e = parse_power();
        if (err_pos) return base;
        return power(base, e, op, exp_start);
    }
    return base;
}

// term := power { ('*' | '/') power }
// A loop (not recursion) makes * and / left-associative: 100 / 7 / 2 is (100 / 7) / 2.
// A single '*' is multiplication; "**" was already taken by parse_power.
static Value parse_term(void) {
    Value l = parse_power();
    while (!err_pos) {
        skip_ws();
        char c = src[pos];
        if (!((c == '*' && src[pos + 1] != '*') || c == '/')) break;
        size_t op = pos;
        pos++;
        skip_ws();
        size_t right_start = pos;
        Value r = parse_power();
        if (err_pos) break;
        // Division by zero is reported at the start of the RIGHT operand, and only after the
        // operand parsed correctly, so an error inside it is found first.
        if (c == '/' && is_zero(r)) {
            fail(right_start);
            break;
        }
        l = arith(c, l, r, op);
    }
    return l;
}

// expr := term { ('+' | '-') term }
// term() is called for the operands, so * and / bind tighter than + and -.
// The loop makes + and - left-associative: 10 - 3 + 1 is (10 - 3) + 1.
static Value parse_expr(void) {
    Value l = parse_term();
    while (!err_pos) {
        skip_ws();
        char c = src[pos];
        if (c != '+' && c != '-') break;
        size_t op = pos;
        pos++;
        Value r = parse_term();
        if (err_pos) break;
        l = arith(c, l, r, op);
    }
    return l;
}

// Evaluates the whole file in src. Returns 0 when it finished (err_pos says whether an error
// happened) and -1 when memory ran out.
static int evaluate(Buf *out) {
    pos = 0;
    err_pos = 0;
    while (pos < len) {
        skip_ws();
        if (pos >= len) break;
        char c = src[pos];
        if (c == '\n') {  // blank line: no output
            pos++;
            continue;
        }
        if (c == '#') {  // '#' as the first non-blank character: the line is a comment.
                         // A '#' anywhere else is never skipped, so it ends up as an invalid
                         // character below.
            while (pos < len && src[pos] != '\n') pos++;
            continue;
        }
        Value v = parse_expr();
        if (err_pos) break;
        skip_ws();
        // After a complete expression only the end of the line may follow. Anything else is
        // an extra ')', an invalid character or a '#', reported at that character.
        if (pos < len && src[pos] != '\n') {
            fail(pos);
            break;
        }
        char tmp[64];
        int n;
        if (v.is_float) n = snprintf(tmp, sizeof tmp, "%.15g\n", v.d);
        else n = snprintf(tmp, sizeof tmp, "%lld\n", v.i);
        if (buf_append(out, tmp, (size_t)n) != 0) return -1;
    }
    return 0;
}

// ----------------------------------- files and folders -----------------------------------

// Input name without folders and without ".txt".
static void split_base(const char *input, const char **base, size_t *blen) {
    const char *slash = strrchr(input, '/');
    const char *b = slash ? slash + 1 : input;
    size_t n = strlen(b);
    if (n > 4 && strcmp(b + n - 4, ".txt") == 0) n -= 4;
    *base = b;
    *blen = n;
}

// Creates the folder if it is missing. An existing folder is fine.
static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    fprintf(stderr, "calc: cannot create output folder %s: %s\n", path, strerror(errno));
    return -1;
}

// Evaluates one input file and writes <outdir>/<base>_<first>_<last>_<id>.txt
static int process_file(const char *input, const char *outdir) {
    src = read_file(input, &len);
    if (!src) {
        fprintf(stderr, "calc: cannot read input file %s\n", input);
        return -1;
    }
    Buf out = {0};
    int status = 0;
    if (evaluate(&out) != 0) {
        fprintf(stderr, "calc: out of memory\n");
        status = -1;
    }
    char *path = NULL;
    FILE *f = NULL;
    if (status == 0) status = ensure_dir(outdir);
    if (status == 0) {
        const char *base;
        size_t bl;
        split_base(input, &base, &bl);
        // snprintf(NULL, 0, ...) measures the exact length first, so the buffer cannot be too
        // small (an earlier hand-counted size was one byte short and cut off the ".txt").
        int need = snprintf(NULL, 0, "%s/%.*s_%s_%s_%s.txt", outdir, (int)bl, base, FIRST, LAST, SID);
        path = malloc((size_t)need + 1);
        if (!path) {
            status = -1;
        } else {
            snprintf(path, (size_t)need + 1, "%s/%.*s_%s_%s_%s.txt", outdir, (int)bl, base, FIRST,
                     LAST, SID);
            f = fopen(path, "wb");
            if (!f) {
                fprintf(stderr, "calc: cannot write %s: %s\n", path, strerror(errno));
                status = -1;
            }
        }
    }
    if (status == 0) {
        int ok;
        if (err_pos) {
            // If anything went wrong the file holds ONLY the first error, no partial results.
            ok = fprintf(f, "ERROR:%zu\n", err_pos) > 0;
        } else {
            ok = out.len == 0 || fwrite(out.data, 1, out.len, f) == out.len;
        }
        if (fclose(f) != 0 || !ok) {
            fprintf(stderr, "calc: cannot write %s\n", path);
            status = -1;
        }
    }
    free(path);
    free(out.data);
    free(src);
    src = NULL;
    return status;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Name of the last folder in a path, used for the default output folder with -d.
// "labs/" gives "labs". For "." and ".." realpath() finds the real name.
static char *folder_label(const char *path) {
    char *copy = strdup(path);
    if (!copy) return NULL;
    size_t n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = '\0';  // drop trailing slashes
    const char *last = strrchr(copy, '/');
    last = last ? last + 1 : copy;
    if (strcmp(last, ".") == 0 || strcmp(last, "..") == 0 || *last == '\0') {
        char *real = realpath(path, NULL);
        if (real) {
            const char *s = strrchr(real, '/');
            s = s ? s + 1 : real;
            char *label = strdup(*s ? s : "root");
            free(real);
            free(copy);
            return label;
        }
    }
    char *label = strdup(last);
    free(copy);
    return label;
}

static void usage(void) {
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char **argv) {
    const char *outdir = NULL, *dir = NULL, *input = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: %s needs a folder name\n", a);
                return 1;
            }
            outdir = argv[++i];
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: %s needs a folder name\n", a);
                return 1;
            }
            dir = argv[++i];
        } else if (a[0] == '-') {
            fprintf(stderr, "calc: unknown option %s\n", a);
            usage();
            return 1;
        } else if (input) {
            fprintf(stderr, "calc: only one input file is allowed\n");
            return 1;
        } else {
            input = a;
        }
    }
    if (!dir && !input) {
        usage();
        return 1;
    }
    if (dir && input) {
        fprintf(stderr, "calc: use either -d DIR or an input file, not both\n");
        return 1;
    }

    // Single file
    if (input) {
        char *auto_out = NULL;
        if (!outdir) {
            // Default output folder: <input_base>_<name>_<studentid>
            const char *base;
            size_t bl;
            split_base(input, &base, &bl);
            int need = snprintf(NULL, 0, "%.*s_%s_%s", (int)bl, base, FIRST, SID);
            auto_out = malloc((size_t)need + 1);
            if (!auto_out) return 1;
            snprintf(auto_out, (size_t)need + 1, "%.*s_%s_%s", (int)bl, base, FIRST, SID);
            outdir = auto_out;
        }
        int st = process_file(input, outdir);
        free(auto_out);
        return st == 0 ? 0 : 1;
    }

    // Folder: every *.txt directly inside it (no subfolders, no other files).
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder %s: %s\n", dir, strerror(errno));
        return 1;
    }
    // The names are collected first and processed afterwards, so that files created in the
    // same folder while we work (when OUTDIR is DIR) are never picked up.
    char **names = NULL;
    size_t cnt = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl <= 4 || strcmp(e->d_name + nl - 4, ".txt") != 0) continue;
        size_t fl = strlen(dir) + 1 + nl + 1;
        char *full = malloc(fl);
        if (!full) break;
        snprintf(full, fl, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {  // ignore subfolders
            free(full);
            continue;
        }
        if (cnt == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char **t = realloc(names, nc * sizeof *names);
            if (!t) {
                free(full);
                break;
            }
            names = t;
            cap = nc;
        }
        names[cnt++] = full;
    }
    closedir(d);
    qsort(names, cnt, sizeof *names, cmp_names);

    char *auto_out = NULL;
    if (!outdir) {
        // Default output folder: <folder name>_<name>_<studentid>
        char *label = folder_label(dir);
        if (!label) return 1;
        int need = snprintf(NULL, 0, "%s_%s_%s", label, FIRST, SID);
        auto_out = malloc((size_t)need + 1);
        if (!auto_out) {
            free(label);
            return 1;
        }
        snprintf(auto_out, (size_t)need + 1, "%s_%s_%s", label, FIRST, SID);
        free(label);
        outdir = auto_out;
    }

    int exit_code = 0;
    if (ensure_dir(outdir) != 0) exit_code = 1;
    for (size_t k = 0; exit_code == 0 && k < cnt; k++) {
        if (process_file(names[k], outdir) != 0) exit_code = 1;
    }
    for (size_t k = 0; k < cnt; k++) free(names[k]);
    free(names);
    free(auto_out);
    return exit_code;
}