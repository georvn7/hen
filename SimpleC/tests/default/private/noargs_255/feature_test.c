/*
    Feature Test with Command Line Test Selection and Parameters
    ===========================================================

    Usage: ./program test1:param1,param2,... test2:param1,param2,...
    
    Available tests and their parameters:
    - arithmetic:a,b (e.g., arithmetic:5,2)
    - relational:a,b (e.g., relational:5,2)
    - logical:a,b (e.g., logical:1,0)
    - bitwise:a,b (e.g., bitwise:6,3)
    - assignments:start_val (e.g., assignments:10)
    - struct_union:x,y,int_val,float_val,char_val (e.g., struct_union:3,4,10,3.14,65)
    - pointers:int_val,float_val (e.g., pointers:10,2.0)
    - enum_switch:color_val (e.g., enum_switch:0 for RED, 1 for GREEN, 2 for BLUE)
    - loops:limit1,limit2,limit3 (e.g., loops:5,2,3)
    - goto:limit (e.g., goto:5)
    - conditional:x,threshold,true_val,false_val (e.g., conditional:5,3,10,20)
    - comma:x_val,y_val (e.g., comma:3,4)
    - sizeof: (no parameters needed)
    - else:initial_val,target_val (e.g., else:0,5)
    - short_char:short_val,char_val (e.g., short_char:30000,100)
    - other: (no parameters needed)
*/

/* ---------- GLOBAL DECLARATIONS ---------- */

enum Color {
    RED,
    GREEN,
    BLUE
};

/* 'static' and 'extern' usage */
static int s_var = 10;
extern int e_var;
int e_var = 5;

/* A simple struct, union, and const, unsigned usage */
struct Point {
    int x;
    int y;
};

union Data {
    int   i;
    float f;
    char  c;
};

const int c_int = 42;
unsigned int u_int = 50U;

/* Parameter structure for each test */
struct TestParams {
    int int_params[10];
    float float_params[5];
    int param_count;
    int float_count;
};

/* ---------- UTILITY FUNCTIONS ---------- */

/* Simple string to integer conversion (no stdlib) */
int simple_atoi(const char *str)
{
    int result = 0;
    int sign = 1;
    int i = 0;

    /* Handle negative numbers */
    if (str[0] == '-') {
        sign = -1;
        i = 1;
    }

    /* Convert digits */
    while (str[i] >= '0' && str[i] <= '9') {
        result = result * 10 + (str[i] - '0');
        i++;
    }

    return sign * result;
}

/* Simple string to float conversion (no stdlib) */
float simple_atof(const char *str)
{
    float result = 0.0f;
    float decimal_part = 0.0f;
    float decimal_divisor = 1.0f;
    int sign = 1;
    int i = 0;
    int decimal_found = 0;

    /* Handle negative numbers */
    if (str[0] == '-') {
        sign = -1;
        i = 1;
    }

    /* Convert digits */
    while (str[i] != '\0') {
        if (str[i] == '.' && !decimal_found) {
            decimal_found = 1;
            i++;
            continue;
        }
        
        if (str[i] >= '0' && str[i] <= '9') {
            if (!decimal_found) {
                result = result * 10.0f + (str[i] - '0');
            } else {
                decimal_divisor *= 10.0f;
                decimal_part = decimal_part * 10.0f + (str[i] - '0');
            }
        }
        i++;
    }

    return sign * (result + decimal_part / decimal_divisor);
}

/* Simple string comparison (no stdlib) */
int simple_strcmp(const char *s1, const char *s2)
{
    int i = 0;
    while (s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) {
            return s1[i] - s2[i];
        }
        i++;
    }
    return s1[i] - s2[i];
}

/* Parse parameters from a string like "5,2,10" */
void parse_params(const char *param_str, struct TestParams *params)
{
    int i = 0;
    int param_start = 0;
    char temp_buf[32];
    int temp_idx = 0;
    
    params->param_count = 0;
    params->float_count = 0;
    
    while (param_str[i] != '\0') {
        if (param_str[i] == ',' || param_str[i+1] == '\0') {
            /* Extract the parameter */
            int j;
            temp_idx = 0;
            for (j = param_start; j <= i && param_str[j] != ',' && param_str[j] != '\0'; j++) {
                temp_buf[temp_idx++] = param_str[j];
            }
            temp_buf[temp_idx] = '\0';
            
            /* Check if it's a float (contains '.' or is explicitly a float) */
            int is_float = 0;
            for (j = 0; j < temp_idx; j++) {
                if (temp_buf[j] == '.') {
                    is_float = 1;
                    break;
                }
            }
            
            if (is_float) {
                params->float_params[params->float_count++] = simple_atof(temp_buf);
            } else {
                params->int_params[params->param_count++] = simple_atoi(temp_buf);
            }
            
            param_start = i + 1;
        }
        i++;
    }
}

/* ---------- TEST FUNCTIONS ---------- */
int test_arithmetic_operators(struct TestParams *params)
{
    int failures = 0;
    int a = (params->param_count > 0) ? params->int_params[0] : 5;
    int b = (params->param_count > 1) ? params->int_params[1] : 2;

    /* Test basic arithmetic operations */
    int sum = a + b;
    int diff = a - b;
    int prod = a * b;
    
    if (sum != (a + b)) failures++;
    if (diff != (a - b)) failures++;
    if (prod != (a * b)) failures++;
    
    if (b != 0) {
        int quot = a / b;
        int rem = a % b;
        if (quot != (a / b)) failures++;
        if (rem != (a % b)) failures++;
    }

    /* Test increment/decrement */
    int original_a = a;
    a++; /* postfix ++ */
    if (a != (original_a + 1)) failures++;

    int original_b = b;
    b--; /* postfix -- */
    if (b != (original_b - 1)) failures++;

    return failures;
}

int test_relational_operators(struct TestParams *params)
{
    int failures = 0;
    int a = (params->param_count > 0) ? params->int_params[0] : 5;
    int b = (params->param_count > 1) ? params->int_params[1] : 2;

    /* Test relational operators with logical consistency */
    int gt = (a > b);
    int lt = (a < b);
    int eq = (a == b);
    int ne = (a != b);
    int ge = (a >= b);
    int le = (a <= b);
    
    /* Logical consistency checks */
    if (eq && ne) failures++;          /* Can't be both equal and not equal */
    if (gt && lt) failures++;          /* Can't be both greater and less */
    if (gt && !ge) failures++;         /* If greater, must be greater-or-equal */
    if (lt && !le) failures++;         /* If less, must be less-or-equal */
    if (eq && (!ge || !le)) failures++; /* If equal, must be both >= and <= */

    return failures;
}

int test_logical_operators(struct TestParams *params)
{
    int failures = 0;
    int a = (params->param_count > 0) ? params->int_params[0] : 1;
    int b = (params->param_count > 1) ? params->int_params[1] : 0;

    /* Test logical operators with meaningful conditions */
    if (a && !a) failures++;           /* This should never execute */
    if (!(!a) != (!!a)) failures++;    /* Double negation should equal original boolean value */
    if ((a || b) && !(a || b)) failures++; /* Can't be both true and false */
    
    /* Test short-circuit evaluation */
    int c = 0;
    if (a || (c = 1)) { /* c should not be set if a is true */ }
    if (a && c != 0) failures++;       /* c should still be 0 if a was true */
    
    if (!a && (c = 2)) { /* c should be set if a is false */ }
    if (!a && c != 2) failures++;      /* c should be 2 if a was false */

    return failures;
}

int test_bitwise_operators(struct TestParams *params)
{
    int failures = 0;
    int a = (params->param_count > 0) ? params->int_params[0] : 6;
    int b = (params->param_count > 1) ? params->int_params[1] : 3;

    /* Test bitwise operations and verify they produce consistent results */
    int and_result = a & b;
    int or_result = a | b;
    int xor_result = a ^ b;
    
    /* Verify bitwise identities */
    if ((a & a) != a) failures++;      /* a & a should equal a */
    if ((a | a) != a) failures++;      /* a | a should equal a */
    if ((a ^ a) != 0) failures++;      /* a ^ a should equal 0 */
    if ((a ^ 0) != a) failures++;      /* a ^ 0 should equal a */

    /* Test that operations are consistent */
    if ((a & b) != and_result) failures++;
    if ((a | b) != or_result) failures++;
    if ((a ^ b) != xor_result) failures++;
    
    /* Verify De Morgan's laws */
    if ((~(a & b)) != ((~a) | (~b))) failures++;
    if ((~(a | b)) != ((~a) & (~b))) failures++;

    /* Test shift operations */
    if (a > 0 && (a << 1) != (a * 2)) failures++; /* Left shift by 1 should double */
    if (a > 0 && (a >> 1) != (a / 2)) failures++; /* Right shift by 1 should halve */

    return failures;
}

int test_assignments(struct TestParams *params)
{
    int failures = 0;
    int x = (params->param_count > 0) ? params->int_params[0] : 10;
    int original_x = x;

    x += 5;
    if (x != (original_x + 5)) failures++;
    
    x -= 3;
    if (x != (original_x + 5 - 3)) failures++;
    
    x = original_x; x *= 2;
    if (x != (original_x * 2)) failures++;
    
    x = original_x;
    if (original_x != 0) {
        x /= 2;
        if (x != (original_x / 2)) failures++;
    }

    x = original_x;
    if (original_x != 0) {
        x %= 7;
        if (x != (original_x % 7)) failures++;
    }

    /* Test bitwise assignment operators */
    x = original_x; x <<= 1;
    if (x != (original_x << 1)) failures++;
    
    x = original_x; x >>= 1;
    if (x != (original_x >> 1)) failures++;

    return failures;
}

int test_struct_and_union(struct TestParams *params)
{
    int failures = 0;
    struct Point p;
    p.x = (params->param_count > 0) ? params->int_params[0] : 3;
    p.y = (params->param_count > 1) ? params->int_params[1] : 4;
    
    /* Test struct member access */
    int expected_sum = p.x + p.y;
    if ((p.x + p.y) != expected_sum) failures++;

    union Data d;
    d.i = (params->param_count > 2) ? params->int_params[2] : 10;
    int saved_int = d.i;
    if (d.i != saved_int) failures++;

    if (params->float_count > 0) {
        d.f = params->float_params[0];
        float saved_float = d.f;
        /* Test that we can read back what we wrote */
        if (d.f != saved_float) failures++;
    }

    char test_char = (params->param_count > 4) ? (char)params->int_params[4] : 'A';
    d.c = test_char;
    if (d.c != test_char) failures++;

    return failures;
}

int test_pointers(struct TestParams *params)
{
    int failures = 0;

    /* & (address-of) and * (dereference) */
    {
        int x = (params->param_count > 0) ? params->int_params[0] : 10;
        int *px = &x;
        if (*px != x) failures++;
        *px = x + 10;
        if (x != (*px)) failures++;
    }

    /* '->' operator on a struct pointer */
    {
        struct Point pt = {
            (params->param_count > 0) ? params->int_params[0] : 1,
            (params->param_count > 1) ? params->int_params[1] : 2
        };
        struct Point *ppt = &pt;
        int new_x = pt.x + 4;
        ppt->x = new_x;
        if (pt.x != new_x) failures++;
    }

    /* pointer to float */
    {
        float f = (params->float_count > 0) ? params->float_params[0] : 2.0f;
        float *pf = &f;
        float original_f = *pf;
        *pf = *pf + 1.14f;
        if (*pf <= original_f) failures++; /* Should be larger */
    }

    return failures;
}

int test_enum_and_switch(struct TestParams *params)
{
    int failures = 0;
    enum Color c = (enum Color)((params->param_count > 0) ? params->int_params[0] : RED);
    int result = 0;

    switch (c) {
    case RED:
        result = 1;
        break;
    case GREEN:
        result = 2;
        break;
    case BLUE:
        result = 3;
        break;
    default:
        result = -1;
        break;
    }

    /* Verify switch worked correctly */
    if (c == RED   && result != 1) failures++;
    if (c == GREEN && result != 2) failures++;
    if (c == BLUE  && result != 3) failures++;

    return failures;
}

int test_loops(struct TestParams *params)
{
    int failures = 0;
    int limit1 = (params->param_count > 0) ? params->int_params[0] : 5;
    int limit2 = (params->param_count > 1) ? params->int_params[1] : 2;
    int limit3 = (params->param_count > 2) ? params->int_params[2] : 3;

    /* for + continue */
    {
        int sum = 0, i;
        int expected_sum = 0;
        for (i = 0; i < limit1; i++) {
            if (i == (limit1 - 2)) continue; /* Skip one iteration */
            sum += i;
            expected_sum += i;
        }
        /* Verify sum is reasonable - at least some iterations happened */
        if (limit1 > 0 && sum < 0) failures++;
    }

    /* while + break */
    {
        int sum = 0, i = 0;
        while (i < limit1) {
            sum += i;
            i++;
            if (i == limit2) break;
        }
        /* Verify we stopped at the right point */
        if (limit2 > 0 && i != limit2) failures++;
    }

    /* do-while */
    {
        int sum = 0, i = 0;
        do {
            sum += i;
            i++;
        } while (i < limit3);
        /* Verify we completed the expected number of iterations */
        if (i != limit3) failures++;
    }

    return failures;
}

/* Goto usage */
int test_goto(struct TestParams *params)
{
    int failures = 0;
    int x = 0;
    int limit = (params->param_count > 0) ? params->int_params[0] : 5;

loop_start:
    x++;
    if (x < limit) {
        goto loop_start;
    }
    if (x != limit) failures++;

    return failures;
}

/* Conditional (?:) usage */
int test_conditional_operator(struct TestParams *params)
{
    int failures = 0;
    int x = (params->param_count > 0) ? params->int_params[0] : 5;
    int threshold = (params->param_count > 1) ? params->int_params[1] : 3;
    int true_val = (params->param_count > 2) ? params->int_params[2] : 10;
    int false_val = (params->param_count > 3) ? params->int_params[3] : 20;
    
    int y = (x > threshold) ? true_val : false_val;
    if (x > threshold && y != true_val) failures++;
    if (x <= threshold && y != false_val) failures++;
    
    return failures;
}

/* Comma operator */
int test_comma_operator(struct TestParams *params)
{
    int failures = 0;
    int x = 0, y = 0;
    int x_val = (params->param_count > 0) ? params->int_params[0] : 3;
    int y_val = (params->param_count > 1) ? params->int_params[1] : 4;
    
    /* Intentionally using comma operator - this warning is expected */
    int z = (x = x_val, y = y_val, x + y); /* final expr => x_val + y_val */
    if (z != (x_val + y_val)) failures++;
    return failures;
}

/* Sizeof operator */
int test_sizeof_operator(struct TestParams *params)
{
    int failures = 0;
    /* Basic sizeof tests - these should never be zero */
    if (sizeof(char) == 0)  failures++;
    if (sizeof(short) == 0) failures++;
    if (sizeof(int) == 0)   failures++;
    if (sizeof(float) == 0) failures++;
    
    /* Test sizeof relationships */
    if (sizeof(char) > sizeof(int)) failures++;
    if (sizeof(short) > sizeof(int)) failures++;
    
    return failures;
}

/* if/else usage */
int test_else_keyword(struct TestParams *params)
{
    int failures = 0;
    int a = (params->param_count > 0) ? params->int_params[0] : 0;
    int b = (params->param_count > 1) ? params->int_params[1] : 0;
    int target = (params->param_count > 1) ? params->int_params[1] : 5;
    
    if (a == b) {
        a = target + 1; /* This should not execute with default params */
    } else {
        a = target;
    }
    if (a != target) failures++;
    return failures;
}

/* Testing short and char specifically */
int test_short_char(struct TestParams *params)
{
    int failures = 0;
    short s = (short)((params->param_count > 0) ? params->int_params[0] : 30000);
    char  c = (char)((params->param_count > 1) ? params->int_params[1] : 100);
    
    /* Test that values are preserved after assignment */
    short s_copy = s;
    char c_copy = c;
    if (s != s_copy) failures++;
    if (c != c_copy) failures++;
    return failures;
}

/* Extra check of static+extern+const+unsigned in one step */
int other_checks(struct TestParams *params)
{
    int failures = 0;
    /* s_var=10, e_var=5 => should add to 15. */
    if ((s_var + e_var) != 15) failures++;

    if (c_int != 42)   failures++;
    if (u_int != 50U)  failures++;
    return failures;
}

/* Test function pointer type */
typedef int (*test_func_param_t)(struct TestParams *);

/* Test registry structure */
struct TestEntry {
    const char *name;
    test_func_param_t func;
    int bit_index;
};

/* ---------- TEST REGISTRY ---------- */

struct TestEntry test_registry[] = {
    {"arithmetic",    test_arithmetic_operators,      0},
    {"relational",    test_relational_operators,      1},
    {"logical",       test_logical_operators,         2},
    {"bitwise",       test_bitwise_operators,         3},
    {"assignments",   test_assignments,               4},
    {"struct_union",  test_struct_and_union,          5},
    {"pointers",      test_pointers,                  6},
    {"enum_switch",   test_enum_and_switch,           7},
    {"loops",         test_loops,                     8},
    {"goto",          test_goto,                      9},
    {"conditional",   test_conditional_operator,      10},
    {"comma",         test_comma_operator,            11},
    {"sizeof",        test_sizeof_operator,           12},
    {"else",          test_else_keyword,              13},
    {"short_char",    test_short_char,                14},
    {"other",         other_checks,                   15},
    {0, 0, 0}  /* sentinel */
};

/* ---------- MAIN WITH COMMAND LINE PARSING ---------- */

int main(int argc, char *argv[])
{
    unsigned int fail_bitmap = 0;
    int i, j;

    /* If no arguments, return error code indicating usage issue */
    if (argc < 2) {
        return 255; /* Special return code for "no tests specified" */
    }

    /* For each command line argument, find and run the corresponding test */
    for (i = 1; i < argc; i++) {
        int test_found = 0;
        char test_name[64];
        const char *param_str = 0;
        int name_len = 0;
        
        /* Split test name and parameters */
        while (argv[i][name_len] != ':' && argv[i][name_len] != '\0') {
            test_name[name_len] = argv[i][name_len];
            name_len++;
        }
        test_name[name_len] = '\0';
        
        if (argv[i][name_len] == ':') {
            param_str = &argv[i][name_len + 1];
        }
        
        /* Search for the test in our registry */
        for (j = 0; test_registry[j].name != 0; j++) {
            if (simple_strcmp(test_name, test_registry[j].name) == 0) {
                test_found = 1;
                
                /* Parse parameters */
                struct TestParams params = {{0}, {0}, 0, 0};
                if (param_str) {
                    parse_params(param_str, &params);
                }
                
                /* Run the test and set the bit if it fails */
                if (test_registry[j].func(&params) != 0) {
                    fail_bitmap |= (1U << test_registry[j].bit_index);
                }
                break;
            }
        }
        
        if (!test_found) {
            return 254; /* Special return code for "unknown test" */
        }
    }

    /* Return the bitmap as exit code */
    return (int)fail_bitmap;
}
