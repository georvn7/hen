/*
  S7 — Multi‑topic runner with structs/pointers/enums (13 tests)

  Tests
  - arithmetic, relational, logical, bitwise, assignments,
    loops, goto, conditional, comma, else,
    struct_union, pointers, enum_switch.

  Behavior
  - Arguments use "name[:params]"; params support comma‑separated ints and floats.
  - Each matched test runs; a non‑zero return marks that test's bit in the result bitmap.
  - Exit codes: 255 (no args), 254 (unknown test); otherwise return the bitmap of failing tests.
 
*/

struct TestParams {
    int   int_params[10];
    float float_params[5];
    int   param_count;
    int   float_count;
};

typedef int (*test_func_param_t)(struct TestParams *);
struct TestEntry { const char *name; test_func_param_t func; int bit_index; };

/* ---- utils ---- */
int simple_strcmp(const char *s1, const char *s2){
    int i=0; while(s1[i]!='\0'&&s2[i]!='\0'){ if(s1[i]!=s2[i]) return (int)((unsigned char)s1[i]-(unsigned char)s2[i]); i++; }
    return (int)((unsigned char)s1[i]-(unsigned char)s2[i]);
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
float simple_atof(const char *s){
    int i=0, sign=1, dec=0; float ip=0.0f, fp=0.0f, div=1.0f;
    if (!s) return 0.0f;
    while (s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n') i++;
    if (s[i]=='+') i++; else if (s[i]=='-') { sign=-1; i++; }

    /* NaN / Inf */
    char a=(char)(s[i]|0x20), b=(char)(s[i+1]|0x20), c=(char)(s[i+2]|0x20);
    if (a=='n' && b=='a' && c=='n'){ volatile float z=0.0f; return z/z; }
    if (a=='i' && b=='n' && c=='f'){ volatile float z=0.0f; return sign>0 ? (1.0f/z) : (-1.0f/z); }

    while (s[i]){
        char ch=s[i];
        if (ch=='.' && !dec){ dec=1; i++; continue; }
        if (ch>='0'&&ch<='9'){ if(!dec){ ip=ip*10.0f+(ch-'0'); } else { fp=fp*10.0f+(ch-'0'); div*=10.0f; } i++; }
        else break;
    }
    float base = ip + (fp/div);
    return sign<0 ? -base : base;
}

void parse_params(const char *ps, struct TestParams *p){
    int i=0,start=0,t=0; char buf[32];
    p->param_count=0; p->float_count=0;
    while (1) {
        char c = ps[i];
        if (c==',' || c=='\0') {
            t=0; int j; for (j=start; j<i && t<(int)sizeof(buf)-1; j++) buf[t++]=ps[j];
            buf[t]='\0';
            if (t>0) {
                int isf=0;
                for (int k=0;k<t;k++){
                    char ch = buf[k];
                    if (ch=='.' || ch=='e' || ch=='E' || ch=='n' || ch=='N' || ch=='i' || ch=='I'){ isf=1; break; }
                }
                if (isf && p->float_count<5) p->float_params[p->float_count++]=simple_atof(buf);
                else if (p->param_count<10)  p->int_params[p->param_count++]=simple_atoi(buf);
            }
            if (c=='\0') break;
            start = i+1;
        }
        i++;
    }
}

/* ---- expressions ---- */
int test_arithmetic_operators(struct TestParams *p){
    int f=0; int a=(p->param_count>0)?p->int_params[0]:5, b=(p->param_count>1)?p->int_params[1]:2;
    if((a+b)!=a+b)f++; if((a-b)!=a-b)f++; if((a*b)!=a*b)f++;
    if(b!=0){ if((a/b)!=a/b)f++; if((a%b)!=a%b)f++; }
    {int x=a; x++; if(x!=a+1)f++;} {int y=b; y--; if(y!=b-1)f++;}
    return f;
}
int test_relational_operators(struct TestParams *p){
    int f=0; int a=(p->param_count>0)?p->int_params[0]:5, b=(p->param_count>1)?p->int_params[1]:2;
    int gt=(a>b),lt=(a<b),eq=(a==b),ne=(a!=b),ge=(a>=b),le=(a<=b);
    if(eq&&ne)f++; if(gt&&lt)f++; if(gt&&!ge)f++; if(lt&&!le)f++; if(eq&&(!ge||!le))f++; return f;
}
int test_logical_operators(struct TestParams *p){
    int f=0; int a=(p->param_count>0)?p->int_params[0]:1, b=(p->param_count>1)?p->int_params[1]:0;
    if(a&&!a)f++; if(!(!a)!=!!a)f++; if((a||b)&&!(a||b))f++;
    {int c=0; if(a||(c=1)){} if(a&&c!=0)f++;} {int c=0; if(!a&&(c=2)){} if(!a&&c!=2)f++;} return f;
}
int test_bitwise_operators(struct TestParams *p){
    int f=0; int a=(p->param_count>0)?p->int_params[0]:6, b=(p->param_count>1)?p->int_params[1]:3;
    if((a&a)!=a)f++; if((a|a)!=a)f++; if((a^a)!=0)f++; if((a^0)!=a)f++;
    if((a&b)!=(a&b))f++; if((a|b)!=(a|b))f++; if((a^b)!=(a^b))f++;
    if((~(a&b))!=((~a)|(~b)))f++; if((~(a|b))!=((~a)&(~b)))f++;
    if(a>0&&(a<<1)!=(a*2))f++; if(a>0&&(a>>1)!=(a/2))f++; return f;
}
int test_assignments(struct TestParams *p){
    int f=0; int x=(p->param_count>0)?p->int_params[0]:10,ox=x;
    x+=5;if(x!=ox+5)f++; x-=3;if(x!=ox+2)f++; x=ox;x*=2;if(x!=ox*2)f++;
    if(ox!=0){x=ox;x/=2;if(x!=ox/2)f++; x=ox;x%=7;if(x!=ox%7)f++;} x=ox;x<<=1;if(x!=(ox<<1))f++; x=ox;x>>=1;if(x!=(ox>>1))f++; return f;
}

/* ---- control flow ---- */
int test_loops(struct TestParams *p){
    int f=0; int limit=(p->param_count>0)?p->int_params[0]:5, stop=(p->param_count>1)?p->int_params[1]:2, do_n=(p->param_count>2)?p->int_params[2]:3;
    int s=0; for(int i=0;i<limit;i++){ if(i==stop+1) continue; s+=i; }
    int exp=0; for(int i=0;i<limit;i++) exp+=i; if(stop+1<limit) exp-=(stop+1); if(s!=exp) f++;
    int i=0; while(i<limit){ i++; if(i==stop) break; } if(i!=(stop<=limit?stop:limit)) f++;
    int d=0,k=0; do{ d++; k++; }while(k<do_n); if(d!=(do_n>0?do_n:1)) f++; return f;
}
int test_goto(struct TestParams *p){
    int f=0; int n=(p->param_count>0)?p->int_params[0]:5; int x=0;
start: x++; if(x<n) goto start; if(x!=n) f++; return f;
}
int test_conditional(struct TestParams *p){
    int f=0; int x=(p->param_count>0)?p->int_params[0]:5, th=(p->param_count>1)?p->int_params[1]:3, tv=(p->param_count>2)?p->int_params[2]:10, fv=(p->param_count>3)?p->int_params[3]:20;
    int y=(x>th)?tv:fv; if(y!=((x>th)?tv:fv)) f++; return f;
}
int test_comma(struct TestParams *p){
    int f=0; int a_in=(p->param_count>0)?p->int_params[0]:3, b_in=(p->param_count>1)?p->int_params[1]:4; int a=0,b=0; int z=(a=a_in,b=b_in,a+b);
    if(z!=a_in+b_in) f++; if(a!=a_in||b!=b_in) f++; return f;
}
int test_else(struct TestParams *p){
    int f=0; int flag=(p->param_count>0)?p->int_params[0]:0, tv=(p->param_count>1)?p->int_params[1]:1, fv=(p->param_count>2)?p->int_params[2]:5;
    int r; if(flag) r=tv; else r=fv; if(r!=(flag?tv:fv)) f++; return f;
}

/* ---- struct/union, pointers, enum/switch ---- */
struct Point { int x; int y; };
union Data  { int i; float f; char c; };

int test_struct_union(struct TestParams *p){
    int f=0;
    int xi=(p->param_count>0)?p->int_params[0]:3;
    int yi=(p->param_count>1)?p->int_params[1]:4;
    float fv=(p->float_count>0)?p->float_params[0]:3.14f;
    int ch=(p->param_count>2)?p->int_params[2]:65;

    struct Point pt; pt.x=xi; pt.y=yi; if(pt.x+pt.y!=xi+yi) f++;
    union Data d; d.i=10; if(d.i!=10) f++; d.f=fv; if(d.f!=fv) f++; d.c=(char)ch; if(d.c!=(char)ch) f++;
    return f;
}

int inc(int x){ return x+1; }

int test_pointers(struct TestParams *p){
    int f=0; int v=(p->param_count>0)?p->int_params[0]:10; float nf=(p->float_count>0)?p->float_params[0]:2.0f;
    { int x=v; int *px=&x; if(*px!=x) f++; *px=x+10; if(x!=*px) f++; }
    { struct Point pt={1,2}; struct Point *pp=&pt; int nx=pp->x+4; pp->x=nx; if(pt.x!=nx) f++; }
    { float t=nf; float *pf=&t; float old=*pf; *pf=*pf+1.25f; if(*pf<=old) f++; }
    { int (*fp)(int)=inc; if(fp(1)!=2) f++; }
    return f;
}

enum Color { RED, GREEN, BLUE };
int test_enum_switch(struct TestParams *p){
    int f=0; int idx=(p->param_count>0)?p->int_params[0]:0; enum Color c=(idx==0)?RED:((idx==1)?GREEN:BLUE);
    int r=0; switch(c){ case RED:r=1;break; case GREEN:r=2;break; case BLUE:r=3;break; default:r=-1; }
    if(r!=((idx==0)?1:((idx==1)?2:3))) f++; return f;
}

/* ---- registry ---- */
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
    {"struct_union",test_struct_union,        10},
    {"pointers",    test_pointers,            11},
    {"enum_switch", test_enum_switch,         12},
    {0,0,0}
};

/* ---- main ---- */
int main(int argc, char *argv[]){
    unsigned int fail_bitmap = 0;
    int page_shift = 0;  /* 0 -> bits 0..7, 8 -> bits 8..15 */
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

        /* control arg: page:<k> (not a test) */
        if (simple_strcmp(test_name, "page") == 0) {
            int k = 0;
            if (param_str) {
                struct TestParams pp = {{0},{0},0,0};
                parse_params(param_str, &pp);
                if (pp.param_count > 0) k = pp.int_params[0];
            }
            if (k < 0) k = 0;
            if (k > 3) k = 3;   /* safety cap */
            page_shift = k * 8;
            continue;           /* don't treat as a test */
        }

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

    return (int)((fail_bitmap >> page_shift) & 0xFF);
}


