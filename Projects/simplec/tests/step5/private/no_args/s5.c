/*
  S5 — Expressions test pack (5 tests)

  Tests
  - arithmetic, relational, logical, bitwise, assignments.

  Behavior
  - Arguments: "name[:comma_separated_ints]".
  - parse_params splits comma-separated integers into TestParams (ints only).
  - Run the named test; if it reports any failures (non-zero), set its bit in the result bitmap.
  - Exit codes: 255 (no args), 254 (unknown test); otherwise return the bitmap of failing tests.

  Private expectation knob (optional third integer per test):
    arithmetic  a,b,exp  -> exp=+1 expect a divisible by b; exp=-1 expect NOT divisible; else ignore.
    relational  a,b,exp  -> exp ∈ {-1,0,+1} expect a<b, a==b, a>b respectively.
    logical     a,b,exp  -> exp=-1 expect both false; exp=0 expect exactly one true; exp=+1 expect both true.
    bitwise     a,b,exp  -> exp=1 expect (a&b)!=0; exp=0 expect (a&b)==0; else ignore.
    assignments x,exp    -> exp=1 expect x even; exp=0 expect x odd; else ignore.
*/

struct TestParams {
    int   int_params[10];
    float float_params[5];
    int   param_count;
    int   float_count;
};

int simple_strcmp(const char *s1, const char *s2) {
    int i = 0;
    while (s1[i] && s2[i] && s1[i] == s2[i]) i++;
    return (unsigned char)s1[i] - (unsigned char)s2[i];
}

int simple_atoi(const char *str) {
    int i = 0, sign = 1, v = 0;
    if (!str) return 0;

    /* skip leading whitespace */
    while (str[i] == ' ' || str[i] == '\t' || str[i] == '\r' || str[i] == '\n') i++;

    /* optional sign */
    if (str[i] == '+') { i++; }
    else if (str[i] == '-') { sign = -1; i++; }

    /* digits */
    while (str[i] >= '0' && str[i] <= '9') {
        v = v * 10 + (str[i] - '0');
        i++;
    }
    return sign * v;
}


void parse_params(const char *param_str, struct TestParams *params) {
    int i = 0, start = 0, t = 0;
    char buf[32];
    params->param_count = 0;
    params->float_count = 0;

    while (1) {
        char c = param_str[i];
        if (c == ',' || c == '\0') {
            t = 0;
            int j;
            for (j = start; j < i && t < (int)sizeof(buf) - 1; j++) buf[t++] = param_str[j];
            buf[t] = '\0';
            if (t > 0) params->int_params[params->param_count++] = simple_atoi(buf);
            if (c == '\0') break;
            start = i + 1;
        }
        i++;
    }
}

/* ---------- tests ---------- */

int test_arithmetic_operators(struct TestParams *p) {
    int failures = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 5;
    int b = (p->param_count > 1) ? p->int_params[1] : 2;
    int has_exp = (p->param_count > 2);
    int exp = has_exp ? p->int_params[2] : 0;

    /* identity checks (tautologies) */
    if ((a + b) != a + b) failures++;
    if ((a - b) != a - b) failures++;
    if ((a * b) != a * b) failures++;
    if (b != 0) {
        if ((a / b) != a / b) failures++;
        if ((a % b) != a % b) failures++;
        /* standard division/modulo consistency */
        if (((a / b) * b + (a % b)) != a) failures++;
    }
    { int x = a; x++; if (x != a + 1) failures++; }
    { int y = b; y--; if (y != b - 1) failures++; }

    /* expectation knob */
    if (has_exp && b != 0) {
        if (exp == 1) { if ((a % b) != 0) failures++; }        /* expect divisible */
        else if (exp == -1) { if ((a % b) == 0) failures++; }  /* expect NOT divisible */
    }
    return failures;
}

int test_relational_operators(struct TestParams *p) {
    int failures = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 5;
    int b = (p->param_count > 1) ? p->int_params[1] : 2;
    int has_exp = (p->param_count > 2);
    int exp = has_exp ? p->int_params[2] : 0; /* -1,0,+1 meaning expected relation of a vs b */

    int gt = (a > b), lt = (a < b), eq = (a == b), ne = (a != b), ge = (a >= b), le = (a <= b);
    if (eq && ne) failures++;
    if (gt && lt) failures++;
    if (gt && !ge) failures++;
    if (lt && !le) failures++;
    if (eq && (!ge || !le)) failures++;

    /* expectation knob: exp ∈ {-1,0,+1} means expect a<b, a==b, a>b respectively */
    if (has_exp) {
        int cmp = 0;
        if (a > b) cmp = 1;
        else if (a < b) cmp = -1;
        else cmp = 0;
        if (cmp != exp) failures++;
    }
    return failures;
}

int test_logical_operators(struct TestParams *p) {
    int failures = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 1;
    int b = (p->param_count > 1) ? p->int_params[1] : 0;
    int has_exp = (p->param_count > 2);
    int exp = has_exp ? p->int_params[2] : 2; /* -1 both false, 0 exactly one true, +1 both true; 2 = ignore */

    if (a && !a) failures++;
    if (!(!a) != (!!a)) failures++;
    if ((a || b) && !(a || b)) failures++;
    /* short-circuit probes */
    { int c = 0; if (a || (c = 1)) { } if (a && c != 0) failures++; }
    { int c = 0; if (!a && (c = 2)) { } if (!a && c != 2) failures++; }

    /* expectation knob based on truthiness of a,b */
    if (has_exp) {
        int A = !!a, B = !!b;
        if (exp == 1) { if (!(A && B)) failures++; }               /* expect both true */
        else if (exp == 0) { if (!((A ^ B) == 1)) failures++; }    /* expect exactly one true */
        else if (exp == -1) { if (!(A == 0 && B == 0)) failures++; } /* expect both false */
    }
    return failures;
}

int test_bitwise_operators(struct TestParams *p) {
    int failures = 0;
    int a = (p->param_count > 0) ? p->int_params[0] : 6;
    int b = (p->param_count > 1) ? p->int_params[1] : 3;
    int has_exp = (p->param_count > 2);
    int exp = has_exp ? p->int_params[2] : 2; /* 1 expect (a&b)!=0, 0 expect (a&b)==0; 2 ignore */

    if ((a & a) != a) failures++;
    if ((a | a) != a) failures++;
    if ((a ^ a) != 0) failures++;
    if ((a ^ 0) != a) failures++;

    if ((a & b) != (a & b)) failures++;
    if ((a | b) != (a | b)) failures++;
    if ((a ^ b) != (a ^ b)) failures++;

    if ((~(a & b)) != ((~a) | (~b))) failures++;
    if ((~(a | b)) != ((~a) & (~b))) failures++;

    if (a > 0 && (a << 1) != (a * 2)) failures++;
    if (a > 0 && (a >> 1) != (a / 2)) failures++;

    /* expectation knob */
    if (has_exp) {
        if (exp == 1) { if ((a & b) == 0) failures++; }
        else if (exp == 0) { if ((a & b) != 0) failures++; }
    }
    return failures;
}

int test_assignments(struct TestParams *p) {
    int failures = 0;
    int x = (p->param_count > 0) ? p->int_params[0] : 10;
    int ox = x;
    int has_exp = (p->param_count > 1);
    int exp = has_exp ? p->int_params[1] : 2; /* 1 expect even ox, 0 expect odd ox; 2 ignore */

    x += 5;  if (x != ox + 5) failures++;
    x -= 3;  if (x != ox + 2) failures++;
    x = ox;  x *= 2; if (x != ox * 2) failures++;
    if (ox != 0) { x = ox; x /= 2; if (x != ox / 2) failures++; }
    if (ox != 0) { x = ox; x %= 7; if (x != ox % 7) failures++; }
    x = ox;  x <<= 1; if (x != (ox << 1)) failures++;
    x = ox;  x >>= 1; if (x != (ox >> 1)) failures++;

    /* expectation knob */
    if (has_exp) {
        if (exp == 1) { if ((ox & 1) != 0) failures++; }      /* expect even */
        else if (exp == 0) { if ((ox & 1) == 0) failures++; } /* expect odd */
    }
    return failures;
}

/* ---------- registry (five tests only) ---------- */

typedef int (*test_func_param_t)(struct TestParams *);
struct TestEntry { const char *name; test_func_param_t func; int bit_index; };

struct TestEntry test_registry[] = {
    {"arithmetic",  test_arithmetic_operators, 0},
    {"relational",  test_relational_operators, 1},
    {"logical",     test_logical_operators,    2},
    {"bitwise",     test_bitwise_operators,    3},
    {"assignments", test_assignments,          4},
    {0, 0, 0}
};

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
        if (argv[i][name_len] == ':') param_str = argv[i] + name_len + 1;

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
