#include <assert.h>
#include <stdio.h>
#include "../calc.c"
void int_to_string(unsigned long n,char *buf) { sprintf(buf,"%lu",n); }
int main(void) {
    int n;char buf[16];
    assert(!calc_eval("(12 + 3)*4",&n) && n==60);
    assert(!calc_eval("-7 / 2 + 8 % 3",&n) && n==-1);
    assert(!calc_eval("-2147483647-1",&n) && n==(-2147483647-1));
    signed_string(n,buf);assert(buf[0]=='-' && buf[10]=='8' && buf[11]==0);
    const char *bad[]={"","1/0","1%0","2147483647+1","50000*50000","(-2147483647-1)/-1","2abc","(1+2","((((((((((((((((((1))))))))))))))))))"};
    for (unsigned int i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(calc_eval(bad[i],&n));
    puts("PASS: calculator precedence, signed arithmetic, bounds and invalid expressions");
}
