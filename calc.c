#include "kernel.h"

/* Checked signed 32-bit integer expressions; no libc or floating point needed. */
struct parser { const char *p; int error, depth; };
static void spaces(struct parser *p) { while (*p->p==' ') p->p++; }
static int expression(struct parser *p);
static int checked(struct parser *p,long long n) {
    if (n>2147483647LL || n<(-2147483647LL-1)) { p->error=1;return 0; }
    return n;
}
static int atom(struct parser *p) {
    spaces(p);
    if (++p->depth>16) { p->error=1;return 0; }
    int n=0;
    if (*p->p=='(') { p->p++;n=expression(p);spaces(p);if (*p->p==')') p->p++;else p->error=1; }
    else if (*p->p=='+' || *p->p=='-') { int neg=*p->p++=='-';n=atom(p);if (neg) n=checked(p,-(long long)n); }
    else if (*p->p>='0' && *p->p<='9') {
        while (*p->p>='0' && *p->p<='9') n=checked(p,(long long)n*10+*p->p++-'0');
    } else p->error=1;
    p->depth--;return n;
}
static int product(struct parser *p) {
    int n=atom(p);
    for (;;) {
        spaces(p);char op=*p->p;if (op!='*' && op!='/' && op!='%') return n;
        p->p++;int r=atom(p);
        if (op=='*') n=checked(p,(long long)n*r);
        else if (!r || (n==(-2147483647-1) && r==-1)) { p->error=1;n=0; }
        else n=op=='/'?n/r:n%r;
    }
}
static int expression(struct parser *p) {
    int n=product(p);
    for (;;) {
        spaces(p);char op=*p->p;if (op!='+' && op!='-') return n;
        p->p++;int r=product(p);n=checked(p,op=='+'?(long long)n+r:(long long)n-r);
    }
}
int calc_eval(const char *s,int *result) {
    struct parser p={s,0,0};int n=expression(&p);spaces(&p);
    if (p.error || *p.p) return -1;
    *result=n;return 0;
}
void signed_string(int n,char *buf) {
    unsigned int magnitude=n<0?0u-(unsigned int)n:(unsigned int)n;
    if (n<0) *buf++='-';
    int_to_string(magnitude,buf);
}
