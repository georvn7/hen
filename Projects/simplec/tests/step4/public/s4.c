/*
  S4 — Function‑pointer registry with minimal parameter parsing

  Behavior
  - Each argument "name[:params]" is matched against a registry of {name, func, bit_index}.
  - parse_params counts the number of '1' characters in params and stores it in TestParams.
  - The selected function is called; if it returns non‑zero, the test's bit is set in fail_bitmap.
  - Exit codes: 255 (no args), 254 (unknown test); otherwise return fail_bitmap.

  Included sample tests
  - "flag": non‑zero if at least one '1' was provided.
  - "gate": non‑zero if two or more '1's were provided.
*/

struct TestParams {
    int   int_params[10];
    float float_params[5];
    int   param_count;
    int   float_count;
};

typedef int (*test_func_param_t)(struct TestParams *);
struct TestEntry { const char *name; test_func_param_t func; int bit_index; };

int simple_strcmp(const char *s1, const char *s2) {
    int i = 0;
    while (s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        i++;
    }
    return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
}

/* Minimal parse: count how many '1' characters appear in param_str. */
void parse_params(const char *param_str, struct TestParams *p) {
    int ones = 0;
    int k = 0;
    p->param_count = 0;
    p->float_count = 0;
    while (param_str && param_str[k] != '\0') {
        if (param_str[k] == '1') ones++;
        k++;
    }
    p->int_params[0] = ones;
    p->param_count   = (ones > 0) ? 1 : 0;
}

/* Two tiny tests to exercise fn-ptr calls.
   - test_any: non-zero if at least one '1'
   - test_atleast2: non-zero if two or more '1's
*/
int test_any(struct TestParams *p)      { return (p->int_params[0] > 0) ? 1 : 0; }
int test_atleast2(struct TestParams *p) { return (p->int_params[0] >= 2) ? 1 : 0; }

/* Sentinel-terminated registry (structs + function pointers). */
struct TestEntry test_registry[] = {
    {"flag", test_any,     0},
    {"gate", test_atleast2,1},
    {0, 0, 0}
};

int main(int argc, char *argv[])
{
    unsigned int fail_bitmap = 0;
    int i, j;

    /* If no arguments, return usage error */
    if (argc < 2) {
        return 255;
    }

    /* For each command line argument, find and (now) run the test */
    for (i = 1; i < argc; i++) {
        int test_found = 0;
        char test_name[64];
        const char *param_str = 0;
        int name_len = 0;

        /* Split test name and parameters (name[:params]) */
        while (argv[i][name_len] != ':' && argv[i][name_len] != '\0') {
            test_name[name_len] = argv[i][name_len];
            name_len++;
        }
        test_name[name_len] = '\0';

        if (argv[i][name_len] == ':') {
            param_str = &argv[i][name_len + 1];
        }

        /* Search for the test in our registry and call through the function pointer */
        for (j = 0; test_registry[j].name != 0; j++) {
            if (simple_strcmp(test_name, test_registry[j].name) == 0) {
                test_found = 1;

                struct TestParams params = {{0}, {0}, 0, 0};
                if (param_str) parse_params(param_str, &params);

                if (test_registry[j].func(&params) != 0) {
                    fail_bitmap |= (1U << test_registry[j].bit_index);
                }
                break;
            }
        }

        if (!test_found) {
            return 254; /* unknown test */
        }
    }

    return (int)fail_bitmap; /* dynamic bitmap */
}
