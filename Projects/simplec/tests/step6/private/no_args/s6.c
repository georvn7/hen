/*
  S6 — Expressions + control‑flow test pack (10 tests)

  Tests
  - arithmetic, relational, logical, bitwise, assignments,
    loops, goto, conditional, comma, else.

  Behavior
  - Arguments: "name[:comma_separated_ints]" (ints only).
  - For each named test, run it; if the test reports failures (non‑zero), set its bit in the result bitmap.
  - Exit codes: 255 (no args), 254 (unknown test); otherwise return the bitmap of failing tests.
*/

struct TestParams {
    int   int_params[10];
    float float_params[5];
    int   param_count;
    int   float_count;
};

/* ---------- tiny utils ---------- */
int simple_strcmp(const char *s1, const char *s2) {
    int i = 0;
    while (s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        i++;
    }
    return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
}
int simple_atoi(const char *s) {
    int i = 0, sign = 1, v = 0;
    if (s[0] == '-') { sign = -1; i = 1; }
    while (s[i] >= '0' && s[i] <= '9') { v = v*10 + (s[i]-'0'); i++; }
    return sign * v;
}
void parse_params(const char *ps, struct TestParams *p) {
    int i = 0, start = 0, t = 0; char buf[32];
    p->param_count = 0; p->float_count = 0;
    while (1) {
        char c = ps[i];
        if (c == ',' || c == '\0') {
            t = 0;
            int j;
            for (j = start; j < i && t < (int)sizeof(buf)-1; j++) buf[t++] = ps[j];
            buf[t] = '\0';
            if (t > 0) p->int_params[p->param_count++] = simple_atoi(buf);
            if (c == '\0') break;
            start = i + 1;
        }
        i++;
    }
}

/* ---------- expressions ---------- */
int test_arithmetic_operators(struct TestParams *p) {
    int f = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 5;
    int b = (p->param_count > 1) ? p->int_params[1] : 2;

    if ((a + b) != a + b) f++;
    if ((a - b) != a - b) f++;
    if ((a * b) != a * b) f++;
    if (b != 0) {
        if ((a / b) != a / b) f++;
        if ((a % b) != a % b) f++;
    }
    { int x = a; x++; if (x != a + 1) f++; }
    { int y = b; y--; if (y != b - 1) f++; }
    return f;
}
int test_relational_operators(struct TestParams *p) {
    int f = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 5;
    int b = (p->param_count > 1) ? p->int_params[1] : 2;

    int gt = (a > b), lt = (a < b), eq = (a == b), ne = (a != b), ge = (a >= b), le = (a <= b);
    if (eq && ne) f++;
    if (gt && lt) f++;
    if (gt && !ge) f++;
    if (lt && !le) f++;
    if (eq && (!ge || !le)) f++;
    return f;
}
int test_logical_operators(struct TestParams *p) {
    int f = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 1;
    int b = (p->param_count > 1) ? p->int_params[1] : 0;

    if (a && !a) f++;
    if (!(!a) != (!!a)) f++;
    if ((a || b) && !(a || b)) f++;
    { int c = 0; if (a || (c = 1)) { } if (a && c != 0) f++; }
    { int c = 0; if (!a && (c = 2)) { } if (!a && c != 2) f++; }
    return f;
}
int test_bitwise_operators(struct TestParams *p) {
    int f = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 6;
    int b = (p->param_count > 1) ? p->int_params[1] : 3;

    if ((a & a) != a) f++;
    if ((a | a) != a) f++;
    if ((a ^ a) != 0) f++;
    if ((a ^ 0) != a) f++;

    if ((a & b) != (a & b)) f++;
    if ((a | b) != (a | b)) f++;
    if ((a ^ b) != (a ^ b)) f++;

    if ((~(a & b)) != ((~a) | (~b))) f++;
    if ((~(a | b)) != ((~a) & (~b))) f++;

    if (a > 0 && (a << 1) != (a * 2)) f++;
    if (a > 0 && (a >> 1) != (a / 2)) f++;
    return f;
}
int test_assignments(struct TestParams *p) {
    int f = 0;
    int x = (p->param_count > 0) ? p->int_params[0] : 10;
    int ox = x;

    x += 5;  if (x != ox + 5) f++;
    x -= 3;  if (x != ox + 2) f++;
    x = ox;  x *= 2; if (x != ox * 2) f++;
    if (ox != 0) { x = ox; x /= 2; if (x != ox / 2) f++; }
    if (ox != 0) { x = ox; x %= 7; if (x != ox % 7) f++; }
    x = ox;  x <<= 1; if (x != (ox << 1)) f++;
    x = ox;  x >>= 1; if (x != (ox >> 1)) f++;
    return f;
}

/* ---------- control flow ---------- */
int test_loops(struct TestParams *p) {
    int f = 0;
    int limit = (p->param_count > 0) ? p->int_params[0] : 5;
    int stop  = (p->param_count > 1) ? p->int_params[1] : 2;
    int do_n  = (p->param_count > 2) ? p->int_params[2] : 3;

    int s = 0;
    for (int i = 0; i < limit; i++) {
        if (i == stop + 1) continue;
        s += i;
    }
    int expected_for = 0; for (int i = 0; i < limit; i++) expected_for += i;
    if (stop + 1 < limit) expected_for -= (stop + 1);
    if (s != expected_for) f++;

    int i = 0;
    while (i < limit) { i++; if (i == stop) break; }
    if (i != (stop <= limit ? stop : limit)) f++;

    int d = 0, k = 0; do { d++; k++; } while (k < do_n);
    if (d != (do_n > 0 ? do_n : 1)) f++;

    return f;
}
int test_goto(struct TestParams *p) {
    int f = 0;
    int n = (p->param_count > 0) ? p->int_params[0] : 5;
    int x = 0;
start:
    x++;
    if (x < n) goto start;
    if (x != n) f++;
    return f;
}
int test_conditional(struct TestParams *p) {
    int f = 0;
    int x  = (p->param_count > 0) ? p->int_params[0] : 5;
    int th = (p->param_count > 1) ? p->int_params[1] : 3;
    int tv = (p->param_count > 2) ? p->int_params[2] : 10;
    int fv = (p->param_count > 3) ? p->int_params[3] : 20;

    int y = (x > th) ? tv : fv;
    int expect = (x > th) ? tv : fv;
    if (y != expect) f++;
    return f;
}
int test_comma(struct TestParams *p) {
    int f = 0;
    int a_in = (p->param_count > 0) ? p->int_params[0] : 3;
    int b_in = (p->param_count > 1) ? p->int_params[1] : 4;
    int a = 0, b = 0;
    int z = (a = a_in, b = b_in, a + b);
    if (z != a_in + b_in) f++;
    if (a != a_in || b != b_in) f++;
    return f;
}
int test_else(struct TestParams *p) {
    int f = 0;
    int flag = (p->param_count > 0) ? p->int_params[0] : 0;
    int tv   = (p->param_count > 1) ? p->int_params[1] : 1;
    int fv   = (p->param_count > 2) ? p->int_params[2] : 5;

    int r;
    if (flag) r = tv; else r = fv;

    int expect = flag ? tv : fv;
    if (r != expect) f++;
    return f;
}

/* ---------- registry (10 entries) ---------- */
typedef int (*test_func_param_t)(struct TestParams *);
struct TestEntry { const char *name; test_func_param_t func; int bit_index; };
struct TestEntry test_registry[] = {
    {"arithmetic",  test_arithmetic_operators, 0},
    {"relational",  test_relational_operators, 1},
    {"logical",     test_logical_operators,    2},
    {"bitwise",     test_bitwise_operators,    3},
    {"assignments", test_assignments,          4},
    {"loops",       test_loops,                5},
    {"goto",        test_goto,                 6},
    {"conditional", test_conditional,          7},
    {"comma",       test_comma,                8},
    {"else",        test_else,                 9},
    {0, 0, 0}
};

/* ---------- main ---------- */
int main(int argc, char *argv[]) {
    unsigned int fail_bitmap = 0;
    int i, j;

    if (argc < 2) return 255;

    for (i = 1; i < argc; i++) {
        int test_found = 0;
        char test_name[64];
        const char *param_str = 0;
        int name_len = 0;

        while (argv[i][name_len] != ':' && argv[i][name_len] != '\0') {
            test_name[name_len] = argv[i][name_len];
            name_len++;
        }
        test_name[name_len] = '\0';
        if (argv[i][name_len] == ':') param_str = &argv[i][name_len + 1];

        for (j = 0; test_registry[j].name != 0; j++) {
            if (simple_strcmp(test_name, test_registry[j].name) == 0) {
                test_found = 1;

                struct TestParams params = {{0},{0},0,0};
                if (param_str) parse_params(param_str, &params);

                if (test_registry[j].func(&params) != 0) {
                    fail_bitmap |= (1U << test_registry[j].bit_index);
                }
                break;
            }
        }
        if (!test_found) return 254;
    }

    return (int)fail_bitmap;
}

