/**
 * Porism 求值内核
 * 
 * 原本是Python写的，但是后面转的C（至于为什么没用C++，e）
 * 我对C不熟悉，很多都是抄文档和抄AI的。
 * 
 * 编译：run.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "porism.h"   /* 统一 ABI 声明 */

// //////////////////////////////////////////////////////////////////
// 线程模型
// //////////////////////////////////////////////////////////////////
typedef struct {
    char errbuf[512];
    char err_type[16];              /* code / kernel / parameter / unknown */
    int err_line, err_col;
    char *outbuf;
    size_t outlen, outcap;
    int out_overflow;
    int json_mode;
    char **outputs;
    int noutputs, output_cap;
    jmp_buf errjmp;
} CallCtx;


#if defined(_WIN32)
// Windows
#include <windows.h>
static SRWLOCK porism_lock = SRWLOCK_INIT;
static INIT_ONCE porism_once = INIT_ONCE_STATIC_INIT;
static DWORD porism_fls = FLS_OUT_OF_INDEXES;
static CallCtx porism_fallback;
static void NTAPI porism_free_ctx(PVOID p) { free(p); }
static BOOL CALLBACK porism_init_fls(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    porism_fls = FlsAlloc(porism_free_ctx);
    return TRUE;
}
static CallCtx *porism_ctx(void) {
    InitOnceExecuteOnce(&porism_once, porism_init_fls, 0, 0);
    if (porism_fls == FLS_OUT_OF_INDEXES) return &porism_fallback;
    CallCtx *c = (CallCtx *)FlsGetValue(porism_fls);
    if (!c) {
        c = (CallCtx *)calloc(1, sizeof(CallCtx));
        if (!c) return &porism_fallback;
        FlsSetValue(porism_fls, c);
    }
    return c;
}
#define PORISM_LOCK()   AcquireSRWLockExclusive(&porism_lock)
#define PORISM_UNLOCK() ReleaseSRWLockExclusive(&porism_lock)
#elif defined(__unix__) || defined(__APPLE__)
// Unix or MacOS
#include <pthread.h>
static pthread_mutex_t porism_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t porism_key;
static pthread_once_t porism_once = PTHREAD_ONCE_INIT;
static CallCtx porism_fallback;
static void porism_free_ctx(void *p) { free(p); }
static void porism_make_key(void) { pthread_key_create(&porism_key, porism_free_ctx); }
static CallCtx *porism_ctx(void) {
    pthread_once(&porism_once, porism_make_key);
    CallCtx *c = (CallCtx *)pthread_getspecific(porism_key);
    if (!c) {
        c = (CallCtx *)calloc(1, sizeof(CallCtx));
        if (!c) return &porism_fallback;
        pthread_setspecific(porism_key, c);
    }
    return c;
}
#define PORISM_LOCK()   pthread_mutex_lock(&porism_lock)
#define PORISM_UNLOCK() pthread_mutex_unlock(&porism_lock)
#else
// Other
static CallCtx porism_fallback;
static CallCtx *porism_ctx(void) { return &porism_fallback; }
#define PORISM_LOCK()   ((void)0)
#define PORISM_UNLOCK() ((void)0)
#endif

#define errbuf       (porism_ctx()->errbuf)
#define err_line     (porism_ctx()->err_line)
#define err_col      (porism_ctx()->err_col)
#define outbuf       (porism_ctx()->outbuf)
#define outlen       (porism_ctx()->outlen)
#define outcap       (porism_ctx()->outcap)
#define out_overflow (porism_ctx()->out_overflow)
#define err_type     (porism_ctx()->err_type)
#define json_mode    (porism_ctx()->json_mode)
#define outputs      (porism_ctx()->outputs)
#define noutputs     (porism_ctx()->noutputs)
#define output_cap   (porism_ctx()->output_cap)

#define MAX_DEPTH 900
#define MAX_NODE_DEPTH 1200
#define MAX_PARSE_DEPTH 500       /* 解析期递归深度：MAX_NODE_DEPTH 只保护 eval */
#define MAX_GVARS 512
#define MAX_GFUNCS 256
#define MAX_PARAMS 24
#define MAX_ARGS 64
#define MAX_PARAMS_TABLE 256
#define MAX_REPEAT_CHARS (1 << 20)

// //////////////////////////////////////////////////////////////////
// 错误信息(我不识English)
// //////////////////////////////////////////////////////////////////
#ifdef PORISM_ZH
#define E_NEED_EXPR   "此处需要表达式，实际得到 %s"
#define E_EXPECT      "期望 %s%s，实际得到 %s"
#define E_BAD_CHAR    "无法识别的字符 %s"
#define E_BAD_ESC     "未知转义序列 \\%c"
#define E_STR_NL      "字符串不能跨行"
#define E_STR_OPEN    "字符串未闭合"
#define E_STR_NUL     "字符串字面量不支持 \\0（内嵌 NUL）"
#define E_UNDEF_VAR   "未定义的变量 %s"
#define E_UNDEF_FN    "未定义的函数 %s"
#define E_ARITY       "函数 %s 需要 %d 个参数，实际传入 %d 个"
#define E_ARITY_RANGE "函数 %s 需要 1~%d 个参数，实际传入 %d 个"
#define E_ARITY_MIN   "函数 %s 至少需要 %d 个参数，实际传入 %d 个"
#define E_NEED_NUM    "运算符 %s 需要数字"
#define E_DIV0        "除以零"
#define E_DEEP        "调用/递归层级过深（>%d）"
#define E_NODE_DEEP   "表达式层级过深（>%d）"
#define E_PARSE_DEEP  "表达式嵌套层级过深（>%d）"
#define E_TRAILING    "语句结束后出现多余内容 %s"
#define E_DUP_PARAM   "函数 %s 的参数名重复"
#define E_RESERVED    "%s 是保留字，不能用作函数名或变量名"
#define E_BAD_PARAM   "参数名必须是标识符"
#define E_EOF         "文件结尾"
#else
#define E_NEED_EXPR   "expected expression, got %s"
#define E_EXPECT      "expected %s%s, got %s"
#define E_BAD_CHAR    "unknown character %s"
#define E_BAD_ESC     "unknown escape \\%c"
#define E_STR_NL      "string cannot span lines"
#define E_STR_OPEN    "unterminated string"
#define E_STR_NUL     "embedded NUL (\\0) is not supported in string literals"
#define E_UNDEF_VAR   "undefined variable %s"
#define E_UNDEF_FN    "undefined function %s"
#define E_ARITY       "function %s needs %d args, got %d"
#define E_ARITY_RANGE "function %s needs 1-%d args, got %d"
#define E_ARITY_MIN   "function %s needs at least %d args, got %d"
#define E_NEED_NUM    "operator %s needs numbers"
#define E_DIV0        "division by zero"
#define E_DEEP        "call depth exceeded (>%d)"
#define E_NODE_DEEP   "expression depth exceeded (>%d)"
#define E_PARSE_DEEP  "expression nesting too deep (>%d)"
#define E_TRAILING    "trailing content after statement: %s"
#define E_DUP_PARAM   "duplicate parameter in function %s"
#define E_RESERVED    "%s is reserved"
#define E_BAD_PARAM   "parameter name must be an identifier"
#define E_EOF         "end of file"
#endif


static void fail_t(const char *type, int line, int col, const char *fmt, ...);
#define fail(line, col, ...) fail_t("code", line, col, __VA_ARGS__)


static char **pkeys, **pvals;
static int nparams;
static void params_reset(void);
static void parse_params_json(const char *text);

// //////////////////////////////////////////////////////////////////
// Arena
// //////////////////////////////////////////////////////////////////
static char *arena;
static size_t arena_used, arena_cap;

static void arena_init(size_t srclen) {
    if (arena) { free(arena); arena = 0; }
    arena_cap = srclen * 24 + (1u << 20);
    arena = (char *)malloc(arena_cap);
    if (!arena) fail_t("kernel", 0, 0, "out of memory");
    arena_used = 0;
}
static void *mem(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (arena_used + n > arena_cap)
        fail_t("kernel", 0, 0, "arena exhausted (source too large / too much string building)");
    void *p = arena + arena_used;
    arena_used += n;
    return p;
}
static char *dupstr(const char *s, size_t n) {
    char *p = (char *)mem(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static char *dupcstr(const char *s) { return dupstr(s, strlen(s)); }

// //////////////////////////////////////////////////////////////////
// 错误
// //////////////////////////////////////////////////////////////////
#define errjmp (porism_ctx()->errjmp)

static void fail_t(const char *type, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof(errbuf), fmt, ap);
    va_end(ap);
    snprintf(err_type, sizeof(err_type), "%s", type);
    err_line = line; err_col = col;
    longjmp(errjmp, 1);
}

// //////////////////////////////////////////////////////////////////
// 值
// //////////////////////////////////////////////////////////////////
enum { V_NUM = 0, V_STR = 1, V_BOOL = 2, V_NULL = 3 };

typedef struct { int k; double n; char *s; int exact; __int128 i; } Val;

#define I128_SAFE ((__int128)1 << 120)
static int i128_fits(__int128 v) { return v <= I128_SAFE && v >= -I128_SAFE; }

static Val vnum(double x)  { Val v; v.k = V_NUM; v.n = x; v.s = 0; v.exact = 0; v.i = 0; return v; }
static Val vint(__int128 x){ Val v; v.k = V_NUM; v.n = (double)x; v.s = 0; v.exact = 1; v.i = x; return v; }
static Val vstr(char *s)   { Val v; v.k = V_STR; v.n = 0; v.s = s; v.exact = 0; v.i = 0; return v; }
static Val vbool(int b)    { Val v; v.k = V_BOOL; v.n = b ? 1 : 0; v.s = 0; v.exact = 0; v.i = 0; return v; }
static Val vnull(void)     { Val v; v.k = V_NULL; v.n = 0; v.s = 0; v.exact = 0; v.i = 0; return v; }

static int truthy(Val v) {
    if (v.k == V_NULL) return 0;
    if (v.k == V_BOOL) return v.n != 0;
    if (v.k == V_NUM)  return v.exact ? v.i != 0 : v.n != 0;
    return v.s && v.s[0] != 0;
}

static char *i128str(__int128 v) {
    char rev[64]; int p = 0, neg = v < 0;
    unsigned __int128 u = neg ? (unsigned __int128)(-v) : (unsigned __int128)v;
    if (!u) rev[p++] = '0';
    while (u) { rev[p++] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg) rev[p++] = '-';
    char *r = (char *)mem((size_t)p + 1);
    for (int j = 0; j < p; j++) r[j] = rev[p - 1 - j];
    r[p] = 0;
    return r;
}

/* 复刻 Python 的 format()：整数值用整数打印，其它用最短往返表示 */
static char *fmtval(Val v) {
    char buf[128];
    if (v.k == V_NULL) return dupcstr("null");
    if (v.k == V_BOOL) return dupcstr(v.n ? "true" : "false");
    if (v.k == V_STR)  return v.s;
    if (v.exact)       return i128str(v.i);
    if (v.n == v.n && v.n != HUGE_VAL && v.n != -HUGE_VAL &&
        floor(v.n) == v.n && fabs(v.n) < 1e16) {
        snprintf(buf, sizeof(buf), "%.0f", v.n);
        return dupcstr(buf);
    }
    if (v.n != v.n)        return dupcstr("nan");
    if (v.n == HUGE_VAL)   return dupcstr("inf");
    if (v.n == -HUGE_VAL)  return dupcstr("-inf");
    for (int p = 15; p <= 17; p++) {
        snprintf(buf, sizeof(buf), "%.*g", p, v.n);
        if (strtod(buf, 0) == v.n) break;
    }
    return dupcstr(buf);
}

static double neednum(Val v, const char *op, int l, int c) {
    if (v.k != V_NUM) fail(l, c, E_NEED_NUM, op);
    return v.n;
}

/* UTF-8码点计数，非法字节按单字节计 */
static size_t utf8_count(const char *s) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        n++;
        if (*p < 0x80) p++;
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) p += 2;
        else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) p += 3;
        else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                 (p[3] & 0xC0) == 0x80) p += 4;
        else p++;
    }
    return n;
}

// //////////////////////////////////////////////////////////////////
// 词法
// //////////////////////////////////////////////////////////////////
enum { T_NUM = 0, T_STR = 1, T_IDENT = 2, T_OP = 3, T_NL = 4, T_EOF = 5,
       T_PARAM = 6 };   /* $KEY参数引用 */
typedef struct { int t; double num; char *s; int line, col; int exact; __int128 i; } Tok;

typedef struct {
    const char *src; size_t i, n; int line, col;
    Tok *toks; int nt, cap;
} Lex;
static Tok *gtoks;
static char *gstrbuf;                 /* 所有字符串字面量共用的临时缓冲，避免按整份源码长度反复分配 */

static void emit_tok(Lex *l, int t, double num, char *s, int line, int col) {
    if (l->nt >= l->cap) fail_t("kernel", line, col, "too many tokens");
    Tok *k = &l->toks[l->nt++];
    memset(k, 0, sizeof(*k));
    k->t = t; k->num = num; k->s = s; k->line = line; k->col = col;
}

static void int_literal(const char *s, size_t len, Tok *k) {
    for (size_t j = 0; j < len; j++)
        if (s[j] == '.' || s[j] == 'e' || s[j] == 'E') return;
    __int128 v = 0;
    for (size_t j = 0; j < len; j++) {
        if (s[j] < '0' || s[j] > '9') return;
        v = v * 10 + (s[j] - '0');
        if (!i128_fits(v)) return;
    }
    k->exact = 1;
    k->i = v;
}
static int peekc(Lex *l, int k) {
    size_t j = l->i + (size_t)k;
    return j < l->n ? (unsigned char)l->src[j] : 0;
}
static int adv(Lex *l) {
    int ch = (unsigned char)l->src[l->i++];
    if (ch == '\n') { l->line++; l->col = 1; }
    else l->col++;
    return ch;
}
static int ident_start(int c) { return isalpha(c) || c == '_' || c >= 128; }
static int ident_char(int c)  { return isalnum(c) || c == '_' || c >= 128; }

static const char *TWO[] = { "**", "//", "==", "!=", "<=", ">=", "&&", "||", 0 };

static Lex lex(const char *src) {
    Lex l;
    memset(&l, 0, sizeof(l));
    l.src = src; l.n = strlen(src); l.line = 1; l.col = 1;
    if (l.n > (size_t)INT_MAX - 2) fail_t("kernel", 0, 0, "source too large");
    l.cap = (int)l.n + 2;
    if (gtoks) { free(gtoks); gtoks = 0; }
    if (gstrbuf) { free(gstrbuf); gstrbuf = 0; }
    l.toks = (Tok *)calloc((size_t)l.cap, sizeof(Tok));
    gtoks = l.toks;
    if (!l.toks) fail_t("kernel", 0, 0, "out of memory");
    gstrbuf = (char *)malloc(l.n + 2);
    if (!gstrbuf) fail_t("kernel", 0, 0, "out of memory");
    while (l.i < l.n) {
        int ch = peekc(&l, 0), line = l.line, col = l.col;
        if (ch == ' ' || ch == '\t' || ch == '\r') { adv(&l); continue; }
        if (ch == '\n') { emit_tok(&l, T_NL, 0, 0, line, col); adv(&l); continue; }
        if (ch == '#') {
            while (peekc(&l, 0) && peekc(&l, 0) != '\n') adv(&l);
            continue;
        }
        if (isdigit(ch) || (ch == '.' && isdigit(peekc(&l, 1)))) {
            char *end;
            const char *start = src + l.i;
            double d = strtod(start, &end);
            size_t len = (size_t)(end - start);
            for (size_t q = 0; q < len; q++) adv(&l);
            int slot = l.nt;
            emit_tok(&l, T_NUM, d, 0, line, col);
            int_literal(start, len, &l.toks[slot]);
            continue;
        }
        if (ch == '$') {
            adv(&l);                       /* $ */
            size_t st = l.i;
            while (peekc(&l, 0) &&
                   (isalnum((unsigned char)peekc(&l, 0)) || peekc(&l, 0) == '_')) adv(&l);
            if (l.i == st || isdigit((unsigned char)l.src[st]))
                fail(line, col, "%s", "parameter reference $ must be followed by a key");
            emit_tok(&l, T_PARAM, 0, dupstr(src + st, l.i - st), line, col);
            continue;
        }
        if (ident_start(ch)) {
            size_t st = l.i;
            while (peekc(&l, 0) && ident_char(peekc(&l, 0))) adv(&l);
            emit_tok(&l, T_IDENT, 0, dupstr(src + st, l.i - st), line, col);
            continue;
        }
        if (ch == '\'' || ch == '"') {
            int q = adv(&l);
            char *buf = gstrbuf;      /* 复用，不占 arena */
            size_t m = 0;
            int closed = 0;
            while (peekc(&l, 0)) {
                int c = adv(&l);
                if (c == q) { closed = 1; break; }
                if (c == '\\') {
                    int e = adv(&l);
                    switch (e) {
                        case 'n': buf[m++] = '\n'; break;
                        case 't': buf[m++] = '\t'; break;
                        case 'r': buf[m++] = '\r'; break;
                        case '0': fail(line, col, "%s", E_STR_NUL); break;
                        case '\\': case '"': case '\'': buf[m++] = (char)e; break;
                        default:
                            fail(line, col, E_BAD_ESC, e);
                    }
                } else if (c == '\n') {
                    fail(line, col, "%s", E_STR_NL);
                } else {
                    buf[m++] = (char)c;
                }
            }
            if (!closed) fail(line, col, "%s", E_STR_OPEN);
            buf[m] = 0;
            emit_tok(&l, T_STR, 0, dupstr(buf, m), line, col);
            continue;
        }
        {
            char pair[3] = { (char)ch, (char)peekc(&l, 1), 0 };
            int found = 0;
            for (int j = 0; TWO[j]; j++) {
                if (!strcmp(pair, TWO[j])) {
                    adv(&l); adv(&l);
                    emit_tok(&l, T_OP, 0, dupcstr(TWO[j]), line, col);
                    found = 1; break;
                }
            }
            if (found) continue;
        }
        if (strchr("+-*/%<>=!(),?:;", ch)) {
            char one[2] = { (char)ch, 0 };
            adv(&l);
            emit_tok(&l, T_OP, 0, dupcstr(one), line, col);
            continue;
        }
        {
            char bad[8]; snprintf(bad, sizeof(bad), "'%c'", ch);
            fail(line, col, E_BAD_CHAR, bad);
        }
    }
    emit_tok(&l, T_EOF, 0, 0, l.line, l.col);
    free(gstrbuf); gstrbuf = 0;
    return l;
}

// //////////////////////////////////////////////////////////////////
// AST
// //////////////////////////////////////////////////////////////////
enum { N_LIT, N_VAR, N_UNARY, N_BINARY, N_COND, N_CALL,
       N_ASSIGN, N_OUTPUT, N_FUNCDEF, N_EXPRSTMT, N_PARAM };
typedef struct Node {
    int t, line, col;
    Val lit;
    char *name, *op;
    struct Node *a, *b, *c;
    struct Node **args; int nargs;
    char **params; int nparams;
    struct Node *next;
} Node;

static Node *node(int t, int line, int col) {
    Node *n = (Node *)mem(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->t = t; n->line = line; n->col = col;
    return n;
}

// //////////////////////////////////////////////////////////////////
// 语法
// //////////////////////////////////////////////////////////////////
typedef struct { Tok *t; int i, n; } P;
static Tok *pk(P *p, int k) { int j = p->i + k; return &p->t[j < p->n ? j : p->n - 1]; }
static Tok *advt(P *p) { Tok *t = pk(p, 0); if (t->t != T_EOF) p->i++; return t; }
static int at(P *p, const char *s) {
    Tok *t = pk(p, 0);
    if (t->t != T_OP && t->t != T_IDENT) return 0;
    return t->s && !strcmp(t->s, s);
}
static char *got(P *p) {
    Tok *t = pk(p, 0);
    if (t->t == T_EOF) return dupcstr(E_EOF);
    if (t->t == T_NL)  return dupcstr("newline");
    if (!t->s) { char b[64]; snprintf(b, sizeof(b), "%g", t->num); return dupcstr(b); }
    char b[128]; snprintf(b, sizeof(b), "'%s'", t->s);
    return dupcstr(b);
}
static Tok *expect(P *p, const char *s, const char *hint) {
    if (!at(p, s)) {
        Tok *t = pk(p, 0);
        fail(t->line, t->col, E_EXPECT, s, hint ? hint : "", got(p));
    }
    return advt(p);
}

static Node *parse_expr(P *p);
static Node *parse_unary(P *p);
static Node *parse_primary_impl(P *p);
static Node *parse_unary_impl(P *p);

/* MAX_NODE_DEPTH 只在 eval 里生效；深层括号在 parser 就会爆栈，必须在这里拦住。
   只在真正产生递归栈帧的两个入口计数，配对进出，扁平的左结合循环不会误报。 */
static int parse_depth;
static void pdepth_enter(int l, int c) {
    if (++parse_depth > MAX_PARSE_DEPTH) {
        parse_depth--;
        fail(l, c, E_PARSE_DEEP, MAX_PARSE_DEPTH);
    }
}

static Node *parse_primary_impl(P *p) {
    Tok *t = pk(p, 0);
    if (t->t == T_NUM) {
        Node *n = node(N_LIT, t->line, t->col);
        n->lit = t->exact ? vint(t->i) : vnum(t->num); advt(p); return n;
    }
    if (t->t == T_STR) {
        Node *n = node(N_LIT, t->line, t->col);
        n->lit = vstr(t->s); advt(p); return n;
    }
    if (t->t == T_PARAM) {
        Node *n = node(N_PARAM, t->line, t->col);
        n->name = t->s; advt(p); return n;
    }
    if (t->t == T_IDENT) {
        char *name = t->s; int l = t->line, c = t->col;
        advt(p);
        /* 必须先拦 Output，否则 Output(1) 会被伪装成普通函数调用 */
        if (!strcmp(name, "Output"))
            fail(l, c, "Output is a statement, not an expression");
        if (at(p, "(")) {
            advt(p);
            Node *n = node(N_CALL, l, c);
            n->name = name;
            n->args = (Node **)mem(sizeof(Node *) * MAX_ARGS);
            n->nargs = 0;
            if (!at(p, ")")) {
                for (;;) {
                    if (n->nargs >= MAX_ARGS) fail(l, c, "too many arguments");
                    n->args[n->nargs++] = parse_expr(p);
                    if (!at(p, ",")) break;
                    advt(p);
                }
            }
            expect(p, ")", " to close argument list");
            return n;
        }
        Node *n = node(N_VAR, l, c);
        n->name = name;
        return n;
    }
    if (at(p, "(")) {
        advt(p);
        Node *n = parse_expr(p);
        expect(p, ")", " to close parenthesis");
        return n;
    }
    fail(t->line, t->col, E_NEED_EXPR, got(p));
    return 0;
}
static Node *parse_primary(P *p) {
    Tok *t = pk(p, 0);
    pdepth_enter(t->line, t->col);
    Node *n = parse_primary_impl(p);
    parse_depth--;
    return n;
}
static Node *parse_power(P *p) {
    Node *base = parse_primary(p);
    if (at(p, "**")) {
        Tok *t = advt(p);
        Node *n = node(N_BINARY, t->line, t->col);
        n->op = "**"; n->a = base; n->b = parse_unary(p);
        return n;
    }
    return base;
}
static Node *parse_unary_impl(P *p) {
    if (at(p, "+") || at(p, "-") || at(p, "!")) {
        Tok *t = advt(p);
        Node *n = node(N_UNARY, t->line, t->col);
        n->op = t->s; n->a = parse_unary(p);
        return n;
    }
    return parse_power(p);
}
static Node *parse_unary(P *p) {
    Tok *t = pk(p, 0);
    pdepth_enter(t->line, t->col);
    Node *n = parse_unary_impl(p);
    parse_depth--;
    return n;
}
static Node *left_assoc(P *p, Node *(*sub)(P *), const char *ops[]) {
    Node *n = sub(p);
    for (;;) {
        int hit = 0;
        for (int j = 0; ops[j]; j++) if (at(p, ops[j])) { hit = 1; break; }
        if (!hit) break;
        Tok *t = advt(p);
        Node *m = node(N_BINARY, t->line, t->col);
        m->op = t->s; m->a = n; m->b = sub(p);
        n = m;
    }
    return n;
}
static Node *p_mul(P *p)  { static const char *o[] = {"*","/","//","%",0}; return left_assoc(p, parse_unary, o); }
static Node *p_add(P *p)  { static const char *o[] = {"+","-",0};           return left_assoc(p, p_mul, o); }
static Node *p_rel(P *p)  { static const char *o[] = {"<","<=",">",">=",0}; return left_assoc(p, p_add, o); }
static Node *p_eq(P *p)   { static const char *o[] = {"==","!=",0};         return left_assoc(p, p_rel, o); }
static Node *p_and(P *p)  { static const char *o[] = {"&&",0};              return left_assoc(p, p_eq, o); }
static Node *p_or(P *p)   { static const char *o[] = {"||",0};              return left_assoc(p, p_and, o); }

static Node *parse_ternary(P *p) {
    Node *c = p_or(p);
    if (at(p, "?")) {
        Tok *t = advt(p);
        Node *n = node(N_COND, t->line, t->col);
        n->a = c; n->b = parse_expr(p);
        expect(p, ":", " (missing : branch)");
        n->c = parse_ternary(p);
        return n;
    }
    return c;
}
static Node *parse_expr(P *p) { return parse_ternary(p); }

static int looks_funcdef(P *p) {
    Tok *t0 = pk(p, 0), *t1 = pk(p, 1);
    if (t0->t != T_IDENT) return 0;
    if (!(t1->t == T_OP && t1->s && !strcmp(t1->s, "("))) return 0;
    int depth = 0;
    for (int j = p->i + 1; j < p->n; j++) {
        Tok *t = &p->t[j];
        if (t->t == T_NL || t->t == T_EOF) return 0;
        if (t->t == T_OP && !strcmp(t->s, "(")) depth++;
        else if (t->t == T_OP && !strcmp(t->s, ")")) {
            if (--depth == 0) {
                Tok *nx = &p->t[j < p->n - 1 ? j + 1 : p->n - 1];
                return nx->t == T_OP && nx->s && !strcmp(nx->s, "=");
            }
        }
    }
    return 0;
}
static int looks_output_stmt(P *p) {
    Tok *t0 = pk(p, 0);
    if (t0->t != T_IDENT || !t0->s || strcmp(t0->s, "Output")) return 0;
    if (!(pk(p,1)->t == T_OP && !strcmp(pk(p,1)->s,"("))) return 0;
    int depth = 0;
    for (int j = p->i + 1; j < p->n; j++) {
        Tok *t = &p->t[j];
        if (t->t == T_NL || t->t == T_EOF) return 0;
        if (t->t == T_OP && !strcmp(t->s, "(")) depth++;
        else if (t->t == T_OP && !strcmp(t->s, ")")) {
            if (--depth == 0) {
                Tok *nx = &p->t[j < p->n - 1 ? j + 1 : p->n - 1];
                return nx->t == T_NL || nx->t == T_EOF ||
                       (nx->t == T_OP && nx->s && !strcmp(nx->s, ";"));
            }
        }
    }
    return 0;
}

static Node *parse_statement(P *p) {
    Tok *first = pk(p, 0);
    if (looks_funcdef(p)) {
        char *name = first->s; int l = first->line, c = first->col;
        if (!strcmp(name, "Output")) fail(l, c, E_RESERVED, name);
        advt(p); advt(p);
        Node *n = node(N_FUNCDEF, l, c);
        n->name = name;
        n->params = (char **)mem(sizeof(char *) * MAX_PARAMS);
        n->nparams = 0;
        if (!at(p, ")")) {
            for (;;) {
                Tok *q = advt(p);
                if (q->t != T_IDENT) fail(q->line, q->col, "%s", E_BAD_PARAM);
                if (!strcmp(q->s, "Output")) fail(q->line, q->col, E_RESERVED, q->s);
                if (n->nparams >= MAX_PARAMS) fail(l, c, "too many parameters");
                n->params[n->nparams++] = q->s;
                if (!at(p, ",")) break;
                advt(p);
            }
        }
        expect(p, ")", " to close parameter list");
        for (int i = 0; i < n->nparams; i++)
            for (int j = i + 1; j < n->nparams; j++)
                if (!strcmp(n->params[i], n->params[j])) fail(l, c, E_DUP_PARAM, name);
        expect(p, "=", " to start function body");
        n->a = parse_expr(p);
        return n;
    }
    if (looks_output_stmt(p)) {
        int l = first->line, c = first->col;
        advt(p); advt(p);
        Node *n = node(N_OUTPUT, l, c);
        n->args = (Node **)mem(sizeof(Node *) * MAX_ARGS);
        n->nargs = 0;
        if (!at(p, ")")) {
            for (;;) {
                if (n->nargs >= MAX_ARGS) fail(l, c, "too many arguments");
                n->args[n->nargs++] = parse_expr(p);
                if (!at(p, ",")) break;
                advt(p);
            }
        }
        expect(p, ")", " to close Output");
        return n;
    }
    if (first->t == T_IDENT && pk(p,1)->t == T_OP &&
        pk(p,1)->s && !strcmp(pk(p,1)->s, "=")) {
        char *name = first->s; int l = first->line, c = first->col;
        /* 常量不可覆盖：原来 pi = 3 / true = 0 会污染全局语义 */
        if (!strcmp(name, "Output") || !strcmp(name, "true") ||
            !strcmp(name, "false") || !strcmp(name, "null") ||
            !strcmp(name, "pi") || !strcmp(name, "e"))
            fail(l, c, E_RESERVED, name);
        advt(p); advt(p);
        Node *n = node(N_ASSIGN, l, c);
        n->name = name; n->a = parse_expr(p);
        return n;
    }
    Node *e = parse_expr(p);
    Node *n = node(N_EXPRSTMT, e->line, e->col);
    n->a = e;
    return n;
}

static Node *parse_program(P *p) {
    Node *head = 0, *tail = 0;
    while (pk(p, 0)->t == T_NL || at(p, ";")) advt(p);
    while (pk(p, 0)->t != T_EOF) {
        Node *s = parse_statement(p);
        if (tail) tail->next = s; else head = s;
        tail = s;
        Tok *nx = pk(p, 0);
        if (nx->t != T_NL && nx->t != T_EOF && !at(p, ";"))
            fail(nx->line, nx->col, E_TRAILING, got(p));
        while (pk(p, 0)->t == T_NL || at(p, ";")) advt(p);
    }
    return head;
}

// //////////////////////////////////////////////////////////////////
// RUNTIM
// //////////////////////////////////////////////////////////////////
typedef struct Bind { char *name; Val v; struct Bind *next; } Bind;
typedef struct { char *name; Val v; } GVar;
typedef struct { char *name; char **params; int np; Node *body; } GFunc;
static GVar gvars[MAX_GVARS]; static int ngvars;
static GFunc gfuncs[MAX_GFUNCS]; static int ngfuncs;
static int node_depth;

static Val lookup(const char *name, Bind *env, int l, int c) {
    for (Bind *b = env; b; b = b->next) if (!strcmp(b->name, name)) return b->v;
    for (int i = 0; i < ngvars; i++) if (!strcmp(gvars[i].name, name)) return gvars[i].v;
    if (!strcmp(name, "true")) return vbool(1);
    if (!strcmp(name, "false")) return vbool(0);
    if (!strcmp(name, "null")) return vnull();
    // 这是两个常量
    if (!strcmp(name, "pi")) return vnum(3.14159265358979323846);
    if (!strcmp(name, "e")) return vnum(2.71828182845904523536);
    char b[128]; snprintf(b, sizeof(b), "'%s'", name);
    fail(l, c, E_UNDEF_VAR, b);
    return vnull();
}
static void setglobal(const char *name, Val v) {
    for (int i = 0; i < ngvars; i++) if (!strcmp(gvars[i].name, name)) { gvars[i].v = v; return; }
    if (ngvars >= MAX_GVARS) fail(0, 0, "too many globals");
    gvars[ngvars].name = dupcstr(name); gvars[ngvars].v = v; ngvars++;
}
static void setfunc(const char *name, char **params, int np, Node *body) {
    for (int i = 0; i < ngfuncs; i++) if (!strcmp(gfuncs[i].name, name)) {
        gfuncs[i].params = params; gfuncs[i].np = np; gfuncs[i].body = body; return;
    }
    if (ngfuncs >= MAX_GFUNCS) fail(0, 0, "too many functions");
    gfuncs[ngfuncs].name = dupcstr(name); gfuncs[ngfuncs].params = params;
    gfuncs[ngfuncs].np = np; gfuncs[ngfuncs].body = body; ngfuncs++;
}

static Val eval(Node *e, Bind *env, int depth);

static Val builtin(const char *name, Val *a, int n, int l, int c) {
    char b[128];
    if (!strcmp(name, "abs"))   { if (n != 1) fail(l, c, E_ARITY, name, 1, n); return vnum(fabs(neednum(a[0], "abs", l, c))); }
    if (!strcmp(name, "sqrt")) {
        if (n != 1) fail(l, c, E_ARITY, name, 1, n);
        double x = neednum(a[0], "sqrt", l, c);
        if (x < 0) fail(l, c, "%s", "sqrt needs a non-negative number");
        return vnum(sqrt(x));
    }
    if (!strcmp(name, "pow"))   {
        if (n != 2) fail(l, c, E_ARITY, name, 2, n);
        double x = neednum(a[0],"pow",l,c), y = neednum(a[1],"pow",l,c);
        double r = pow(x, y);
        if (r != r) fail(l, c, "%s", "math domain error");
        return vnum(r);
    }
    if (!strcmp(name, "min") || !strcmp(name, "max")) {
        if (n < 1) fail(l, c, E_ARITY_MIN, name, 1, n);
        double r = neednum(a[0], name, l, c);
        for (int i = 1; i < n; i++) {
            double x = neednum(a[i], name, l, c);
            r = !strcmp(name, "min") ? (x < r ? x : r) : (x > r ? x : r);
        }
        return vnum(r);
    }
    if (!strcmp(name, "round")) {
        if (n < 1 || n > 2) fail(l, c, E_ARITY_RANGE, name, 2, n);
        double x = neednum(a[0], "round", l, c);
        double ndd = n == 2 ? neednum(a[1], "round", l, c) : 0;
        if (ndd != ndd || fabs(ndd) > 100000.0)
            fail(l, c, "%s", "round: ndigits out of range");
        int nd = (int)ndd;
        double f, frac, r;
        if (nd <= 0) {
            double p = pow(10.0, (double)(-nd));
            if (!isfinite(p)) return vnum(0.0);
            double q = x / p;
            if (!isfinite(q)) return vnum(x);
            f = floor(q); frac = q - f;
            r = frac > 0.5 ? f + 1 : frac < 0.5 ? f
              : (fmod(f, 2.0) == 0.0 ? f : f + 1);
            double res = r * p;
            if (!isfinite(res) || fabs(res) >= (double)I128_SAFE) return vnum(res);
            return vint((__int128)res);
        }
        if (nd > 600) return vnum(x);
        char tmp[1024];                     // 最大1.8e308: 309位整数+600 位小数
        snprintf(tmp, sizeof(tmp), "%.*f", nd, x);
        double res = strtod(tmp, 0);
        if (isfinite(res) && fabs(res) < (double)I128_SAFE && floor(res) == res)
            return vint((__int128)res);
        return vnum(res);
    }
    if (!strcmp(name, "num")) {     // 把 int 改成了 num
        if (n != 1) fail(l, c, E_ARITY, name, 1, n);
        Val v = a[0];
        if (v.k == V_NUM) {
            if (v.exact) return vint(v.i);
            double x = v.n;
            if (!isfinite(x) || fabs(x) >= 9.22e18)    // LLONG_MAX 附近不再安全
                fail(l, c, "%s", "num needs a number in 64-bit range");
            return vint((__int128)(long long)x);
        }
        if (v.k == V_BOOL) return vint(v.n ? 1 : 0);
        if (v.k == V_STR) {
            char *end;
            errno = 0;
            long long q = strtoll(v.s, &end, 10);
            if (errno == ERANGE) fail(l, c, "%s", "num needs a number in 64-bit range");
            while (*end && isspace((unsigned char)*end)) end++;
            if (end == v.s || *end) fail(l, c, "%s", "num needs a number");
            return vint((__int128)q);
        }
        fail(l, c, "%s", "num needs a number");
    }
    if (!strcmp(name, "float")) {
        if (n != 1) fail(l, c, E_ARITY, name, 1, n);
        Val v = a[0];
        if (v.k == V_NUM || v.k == V_BOOL) return vnum(v.n);
        if (v.k == V_STR) {
            char *end;
            double d = strtod(v.s, &end);
            while (*end && isspace((unsigned char)*end)) end++;
            if (end == v.s || *end) fail(l, c, "%s", "float needs a number");
            return vnum(d);
        }
        fail(l, c, "%s", "float needs a number");
    }
    if (!strcmp(name, "str"))   { if (n != 1) fail(l, c, E_ARITY, name, 1, n); return vstr(fmtval(a[0])); }
    if (!strcmp(name, "len"))   { if (n != 1) fail(l, c, E_ARITY, name, 1, n);
        if (a[0].k != V_STR) fail(l, c, "len needs a string");
        return vint((__int128)utf8_count(a[0].s)); }
    if (!strcmp(name, "upper") || !strcmp(name, "lower")) {
        if (n != 1) fail(l, c, E_ARITY, name, 1, n);
        char *s = fmtval(a[0]);
        char *r = dupstr(s, strlen(s));
        for (char *q = r; *q; q++) {
            if (!strcmp(name, "upper")) *q = (char)toupper((unsigned char)*q);
            else *q = (char)tolower((unsigned char)*q);
        }
        return vstr(r);
    }
    snprintf(b, sizeof(b), "'%s'", name);
    fail(l, c, E_UNDEF_FN, b);
    return vnull();
}

static Val eval_call(Node *e, Bind *env, int depth) {
    if (depth >= MAX_DEPTH) fail(e->line, e->col, E_DEEP, MAX_DEPTH);
    if (e->nargs > MAX_ARGS)
        fail(e->line, e->col, "too many arguments (max %d)", MAX_ARGS);
    Val a[MAX_ARGS > 0 ? (e->nargs ? e->nargs : 1) : 1];
    for (int i = 0; i < e->nargs; i++) a[i] = eval(e->args[i], env, depth + 1);
    for (int i = 0; i < ngfuncs; i++) {
        GFunc *f = &gfuncs[i];
        if (!strcmp(f->name, e->name)) {
            if (e->nargs != f->np) fail(e->line, e->col, E_ARITY, e->name, f->np, e->nargs);
            Bind binds[f->np ? f->np : 1];
            Bind *local = 0;
            for (int j = 0; j < f->np; j++) {
                binds[j].name = f->params[j]; binds[j].v = a[j];
                binds[j].next = local; local = &binds[j];
            }
            /* 表达式深度按 单次求值 计，不能与调用深度累加，否则深层递归会被误判成 AST 过深。 */
            int saved = node_depth;
            node_depth = 0;
            Val r = eval(f->body, local, depth + 1);
            node_depth = saved;
            return r;
        }
    }
    return builtin(e->name, a, e->nargs, e->line, e->col);
}

/* 有界精确整数运算 */
static int exact_arith(const char *op, Val x, Val y, Val *out, int l, int c) {
    if (x.k != V_NUM || y.k != V_NUM || !x.exact || !y.exact) return 0;
    if (!strcmp(op, "/")) return 0;              /* Python: int / int -> float */
    __int128 a = x.i, b = y.i, z;
    if (!strcmp(op, "+") || !strcmp(op, "-")) {
        z = !strcmp(op, "+") ? a + b : a - b;
        *out = i128_fits(z) ? vint(z) : vnum((double)z);
        return 1;
    }
    if (!strcmp(op, "*")) {
        /* 两个都 <=2^60 时乘积必 <=2^120，不会溢出 __int128 */
        __int128 aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
        if (aa > ((__int128)1 << 60) || bb > ((__int128)1 << 60)) return 0;
        *out = vint(a * b);
        return 1;
    }
    if (!strcmp(op, "//") || !strcmp(op, "%")) {
        if (b == 0) fail(l, c, "%s", E_DIV0);
        __int128 q = a / b, rem = a % b;
        if (rem != 0 && ((rem > 0) != (b > 0))) q--;   /* 向下取整，不是向零 */
        *out = vint(!strcmp(op, "//") ? q : a - q * b);
        return 1;
    }
    if (!strcmp(op, "**")) {
        if (b < 0 || b > 1000) return 0;
        __int128 t = 1, aa = a < 0 ? -a : a;
        for (__int128 j = 0; j < b; j++) {
            __int128 ta = t < 0 ? -t : t;
            if (aa && ta > I128_SAFE / aa) return 0;
            t *= a;
        }
        *out = vint(t);
        return 1;
    }
    return 0;
}

static Val eval(Node *e, Bind *env, int depth) {
    if (++node_depth > MAX_NODE_DEPTH) {
        node_depth--; fail(e->line, e->col, E_NODE_DEEP, MAX_NODE_DEPTH);
    }
    Val r = vnull();
    switch (e->t) {
        case N_LIT: r = e->lit; break;
        case N_VAR: r = lookup(e->name, env, e->line, e->col); break;
        case N_PARAM: {
            int found = 0;
            for (int i = 0; i < nparams; i++)
                if (!strcmp(pkeys[i], e->name)) { r = vstr(pvals[i]); found = 1; break; }
            if (!found)
                fail_t("parameter", e->line, e->col,
                       "parameter $%s not provided by -p", e->name);
            break;
        }
        case N_UNARY: {
            Val v = eval(e->a, env, depth);
            if (!strcmp(e->op, "!")) r = vbool(!truthy(v));
            else if (v.k == V_NUM && v.exact)
                   r = vint(!strcmp(e->op, "-") ? -v.i : v.i);
            else { double x = neednum(v, e->op, e->line, e->col);
                   r = vnum(!strcmp(e->op, "-") ? -x : x); }
            break;
        }
        case N_COND: {
            Val c = eval(e->a, env, depth);
            r = eval(truthy(c) ? e->b : e->c, env, depth);
            break;
        }
        case N_BINARY: {
            const char *op = e->op;
            if (!strcmp(op, "&&")) { r = vbool(truthy(eval(e->a,env,depth)) && truthy(eval(e->b,env,depth))); break; }
            if (!strcmp(op, "||")) { r = vbool(truthy(eval(e->a,env,depth)) || truthy(eval(e->b,env,depth))); break; }
            Val x = eval(e->a, env, depth), y = eval(e->b, env, depth);
            if (!strcmp(op, "==") || !strcmp(op, "!=")) {
                int eq;
                if (x.k == V_STR && y.k == V_STR) eq = !strcmp(x.s, y.s);
                else if (x.k == V_NULL || y.k == V_NULL) eq = (x.k == V_NULL && y.k == V_NULL);
                else if (x.k == V_NUM && y.k == V_NUM && x.exact && y.exact) eq = (x.i == y.i);
                else eq = (neednum(x,op,e->line,e->col) == neednum(y,op,e->line,e->col));
                r = vbool(!strcmp(op, "==") ? eq : !eq);
                break;
            }
            if (!strcmp(op,"<") || !strcmp(op,"<=") || !strcmp(op,">") || !strcmp(op,">=")) {
                if (x.k == V_STR && y.k == V_STR) {
                    int c = strcmp(x.s, y.s);
                    r = vbool(!strcmp(op,"<") ? c<0 : !strcmp(op,"<=") ? c<=0 : !strcmp(op,">") ? c>0 : c>=0);
                } else if (x.k == V_NUM && y.k == V_NUM && x.exact && y.exact) {
                    __int128 a = x.i, b = y.i;
                    r = vbool(!strcmp(op,"<") ? a<b : !strcmp(op,"<=") ? a<=b : !strcmp(op,">") ? a>b : a>=b);
                } else {
                    double p = neednum(x,op,e->line,e->col), q = neednum(y,op,e->line,e->col);
                    r = vbool(!strcmp(op,"<") ? p<q : !strcmp(op,"<=") ? p<=q : !strcmp(op,">") ? p>q : p>=q);
                }
                break;
            }
            if (!strcmp(op, "+") && (x.k == V_STR || y.k == V_STR)) {
                char *sx = fmtval(x), *sy = fmtval(y);
                char *buf = (char *)mem(strlen(sx) + strlen(sy) + 1);
                strcpy(buf, sx); strcat(buf, sy);
                r = vstr(buf); break;
            }
            if (!strcmp(op, "*") && ((x.k == V_STR && y.k == V_NUM) ||
                                     (y.k == V_STR && x.k == V_NUM))) {
                Val s = x.k == V_STR ? x : y;
                Val n = x.k == V_STR ? y : x;
                double d = n.exact ? (double)n.i : n.n;
                if (!(d == d) || d > HUGE_VAL || d < -HUGE_VAL || floor(d) != d)
                    fail(e->line, e->col, "%s", "string repeat count must be an integer value");
                if (d <= 0) { r = vstr(dupcstr("")); break; }
                size_t sl = strlen(s.s);
                if (d > (double)(MAX_REPEAT_CHARS / (sl ? sl : 1)) + 1)
                    fail_t("kernel", e->line, e->col, "%s", "string repeat result too large");
                long long k = (long long)d;
                char *buf = (char *)mem(sl * (size_t)k + 1);
                char *dst = buf;
                for (long long i = 0; i < k; i++) { memcpy(dst, s.s, sl); dst += sl; }
                *dst = 0;
                r = vstr(buf); break;
            }
            if (exact_arith(op, x, y, &r, e->line, e->col)) break;
            {
                double p = neednum(x,op,e->line,e->col), q = neednum(y,op,e->line,e->col);
                if ((!strcmp(op,"/") || !strcmp(op,"//") || !strcmp(op,"%")) && q == 0)
                    fail(e->line, e->col, "%s", E_DIV0);
                if (!strcmp(op,"+")) r = vnum(p+q);
                else if (!strcmp(op,"-")) r = vnum(p-q);
                else if (!strcmp(op,"*")) r = vnum(p*q);
                else if (!strcmp(op,"/")) r = vnum(p/q);
                else if (!strcmp(op,"//")) r = vnum(floor(p/q));
                else if (!strcmp(op,"%")) r = vnum(p - floor(p/q)*q);
                else if (!strcmp(op,"**")) {
                    double z = pow(p, q);
                    if (z != z) fail(e->line, e->col, "%s", "math domain error");
                    r = vnum(z);
                }
                else fail(e->line, e->col, "unknown operator %s", op);
            }
            break;
        }
        case N_CALL: r = eval_call(e, env, depth); break;
        default: fail(e->line, e->col, "internal: bad node"); break;
    }
    node_depth--;
    return r;
}

// //////////////////////////////////////////////////////////////////
// 输出
// //////////////////////////////////////////////////////////////////
static void emit(const char *s) {
    if (json_mode) {
        if (noutputs < output_cap) outputs[noutputs++] = dupcstr(s);
        return;
    }
    if (!outbuf) { printf("%s\n", s); return; }
    size_t n = strlen(s);
    if (outlen + n + 2 > outcap) { out_overflow = 1; return; }  /* 不静默截断 */
    memcpy(outbuf + outlen, s, n);
    outlen += n;
    outbuf[outlen++] = '\n';
    outbuf[outlen] = 0;
}

/* 重置 arena/变量表/函数表，注入宿主变量 */
static void prepare(size_t srclen, const char **names, const double *vals, int nvars) {
    arena_init(srclen + 256);
    ngvars = ngfuncs = node_depth = parse_depth = 0;
    params_reset();
    for (int i = 0; i < nvars; i++) setglobal(names[i], vnum(vals[i]));
}

static void run(const char *src, const char *params_json) {
    prepare(strlen(src), 0, 0, 0);
    parse_params_json(params_json);
    Lex l = lex(src);
    P p; p.t = l.toks; p.i = 0; p.n = l.nt;
    Node *st = parse_program(&p);
    output_cap = l.nt + 4;
    outputs = (char **)mem(sizeof(char *) * (size_t)output_cap);
    noutputs = 0;
    for (; st; st = st->next) {
        switch (st->t) {
            case N_FUNCDEF:
                setfunc(st->name, st->params, st->nparams, st->a);
                break;
            case N_ASSIGN:
                setglobal(st->name, eval(st->a, 0, 0));
                break;
            case N_OUTPUT:
                if (st->nargs != 1) fail(st->line, st->col, "Output needs 1 argument, got %d", st->nargs);
                emit(fmtval(eval(st->args[0], 0, 0)));
                break;
            case N_EXPRSTMT:
                eval(st->a, 0, 0);
                break;
        }
    }
}

// //////////////////////////////////////////////////////////////////
// 平铺参数对象 / 结果对象
// //////////////////////////////////////////////////////////////////
static void params_reset(void) {
    nparams = 0;
    pkeys = (char **)mem(sizeof(char *) * MAX_PARAMS_TABLE);
    pvals = (char **)mem(sizeof(char *) * MAX_PARAMS_TABLE);
}

static int valid_param_key(const char *k) {
    if (!k || !((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= 'a' && k[0] <= 'z') || k[0] == '_'))
        return 0;
    for (const char *q = k + 1; *q; q++)
        if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
              (*q >= '0' && *q <= '9') || *q == '_')) return 0;
    return 1;
}

static void jskip(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') (*p)++;
}
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* 解析 JSON 字符串，返回 arena 内的 C 串；失败按 parameter 分类。 */
static char *jstring(const char **p) {
    if (**p != '"') fail_t("parameter", 0, 0, "%s", "parameter: expected a JSON string");
    (*p)++;
    char *buf = (char *)mem(strlen(*p) + 8);
    size_t m = 0;
    while (**p && **p != '"') {
        int c = (unsigned char)*(*p);
        if (c == '\\') {
            (*p)++;
            int e = (unsigned char)*(*p);
            if (!e) fail_t("parameter", 0, 0, "%s", "parameter: bad string escape");
            switch (e) {
                case 'n': buf[m++] = '\n'; (*p)++; break;
                case 't': buf[m++] = '\t'; (*p)++; break;
                case 'r': buf[m++] = '\r'; (*p)++; break;
                case 'b': buf[m++] = '\b'; (*p)++; break;
                case 'f': buf[m++] = '\f'; (*p)++; break;
                case '"': case '\\': case '/': buf[m++] = (char)e; (*p)++; break;
                case 'u': {
                    (*p)++;
                    unsigned int cp = 0;
                    for (int j = 0; j < 4; j++) {
                        int h = hexval((unsigned char)(*p)[j]);
                        if (h < 0) fail_t("parameter", 0, 0, "%s", "parameter: bad \\u escape");
                        cp = cp * 16 + (unsigned int)h;
                    }
                    (*p) += 4;
                    /* 高代理+紧跟的低代理合并成一个码点，孤立代理退化为 "?" */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        (*p)[0] == '\\' && (*p)[1] == 'u') {
                        unsigned int lo = 0; int ok = 1;
                        for (int j = 0; j < 4; j++) {
                            int h = hexval((unsigned char)(*p)[2 + j]);
                            if (h < 0) { ok = 0; break; }
                            lo = lo * 16 + (unsigned int)h;
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            *p += 6;
                        }
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) buf[m++] = '?';
                    else if (cp < 0x80) buf[m++] = (char)cp;
                    else if (cp < 0x800) {
                        buf[m++] = (char)(0xC0 | (cp >> 6));
                        buf[m++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        buf[m++] = (char)(0xE0 | (cp >> 12));
                        buf[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[m++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[m++] = (char)(0xF0 | (cp >> 18));
                        buf[m++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[m++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: fail_t("parameter", 0, 0, "parameter: bad string escape \\%c", e);
            }
        } else {
            buf[m++] = (char)c; (*p)++;
        }
    }
    if (**p != '"') fail_t("parameter", 0, 0, "%s", "parameter: unterminated JSON string");
    (*p)++;
    buf[m] = 0;
    return buf;
}

static void params_put(const char *key, const char *val) {
    for (int i = 0; i < nparams; i++)
        if (!strcmp(pkeys[i], key)) { pvals[i] = (char *)val; return; }
    if (nparams >= MAX_PARAMS_TABLE)
        fail_t("parameter", 0, 0, "%s", "parameter: too many parameters");
    pkeys[nparams] = (char *)key;
    pvals[nparams] = (char *)val;
    nparams++;
}

static void parse_params_json(const char *text) {
    if (!text || !*text) return;
    const char *p = text;
    jskip(&p);
    if (*p++ != '{') fail_t("parameter", 0, 0, "%s", "parameter: top level must be a JSON object");
    jskip(&p);
    if (*p == '}') {
        p++; jskip(&p);
        if (*p) fail_t("parameter", 0, 0, "%s", "parameter: trailing content after object");
        return;
    }
    for (;;) {
        jskip(&p);
        char *key = jstring(&p);
        if (!valid_param_key(key))
            fail_t("parameter", 0, 0, "parameter: invalid parameter key '%s'", key);
        jskip(&p);
        if (*p++ != ':') fail_t("parameter", 0, 0, "%s", "parameter: expected ':' after key");
        jskip(&p);
        if (*p != '"')
            fail_t("parameter", 0, 0, "%s", "parameter: values must be strings (flat object)");
        char *val = jstring(&p);
        params_put(key, val);
        jskip(&p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        fail_t("parameter", 0, 0, "%s", "parameter: expected ',' or '}'");
    }
    jskip(&p);
    if (*p) fail_t("parameter", 0, 0, "%s", "parameter: trailing content after object");
}

/* JSON 结果对象序列化 */
static size_t json_esc_len(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\t' ||
            *p == '\r' || *p == '\b' || *p == '\f') n += 2;
        else if (*p < 0x20) n += 6;
        else n++;
    }
    return n;
}
static char *json_esc(char *d, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  *d++ = '\\'; *d++ = '"'; break;
            case '\\': *d++ = '\\'; *d++ = '\\'; break;
            case '\n': *d++ = '\\'; *d++ = 'n'; break;
            case '\t': *d++ = '\\'; *d++ = 't'; break;
            case '\r': *d++ = '\\'; *d++ = 'r'; break;
            case '\b': *d++ = '\\'; *d++ = 'b'; break;
            case '\f': *d++ = '\\'; *d++ = 'f'; break;
            default:
                if (*p < 0x20) { snprintf(d, 7, "\\u%04x", (unsigned)*p); d += 6; }
                else *d++ = (char)*p;
        }
    }
    return d;
}
static char *jappend(char *d, const char *s) {
    size_t n = strlen(s);
    memcpy(d, s, n);
    return d + n;
}

static int write_result_json(int is_error) {
    char num[128];
    char head[256];
    size_t total;
    if (is_error) {
        snprintf(head, sizeof(head),
                 "{\"status\":\"error\",\"errors\":[{\"id\":1,\"type\":\"%s\",\"message\":",
                 err_type);
        total = strlen(head) + 2 + json_esc_len(errbuf);   /* 引号+内容 */
        snprintf(num, sizeof(num), ",\"line\":%d,\"col\":%d}],\"outputs\":[",
                 err_line, err_col);
        total += strlen(num);
    } else {
        total = strlen("{\"status\":\"ok\",\"errors\":null,\"outputs\":[");
    }
    for (int i = 0; i < noutputs; i++) {
        if (i) total++;                       /* ',' */
        total += 2 + json_esc_len(outputs[i]);
    }
    total += 2;                              /* ]} */
    /* 改用堆，并在所有返回点释放。 */
    char *b = (char *)malloc(total + 1);
    if (!b) {                                /* 堆也失败：写最小结果，绝不 longjmp */
        static const char oom[] =
            "{\"status\":\"error\",\"errors\":[{\"id\":1,\"type\":\"kernel\","
            "\"message\":\"out of memory\",\"line\":0,\"col\":0}],\"outputs\":[]}";
        size_t ol = strlen(oom);
        if (!outbuf) { fputs(oom, stdout); fputc('\n', stdout); }
        else if (outbuf && outcap > ol) { memcpy(outbuf, oom, ol + 1); outlen = ol; }
        else { out_overflow = 1; if (outcap) outbuf[0] = 0; }
        return 1;
    }
    char *d = b;
    if (is_error) d = jappend(d, head);
    else d = jappend(d, "{\"status\":\"ok\",\"errors\":null,\"outputs\":[");
    if (is_error) {
        *d++ = '"'; d = json_esc(d, errbuf); *d++ = '"';
        d = jappend(d, num);
    }
    for (int i = 0; i < noutputs; i++) {
        if (i) *d++ = ',';
        *d++ = '"'; d = json_esc(d, outputs[i]); *d++ = '"';
    }
    *d++ = ']'; *d++ = '}'; *d = 0;
    size_t len = (size_t)(d - b);
    int rc = is_error ? 1 : 0;
    if (!outbuf) { fputs(b, stdout); fputc('\n', stdout); free(b); return rc; }
    if (len + 1 > outcap) {
        out_overflow = 1;
        snprintf(err_type, sizeof(err_type), "kernel");
        snprintf(errbuf, sizeof(errbuf), "result object buffer too small");
        if (outcap) outbuf[0] = 0;
        free(b);
        return 2;
    }
    memcpy(outbuf, b, len + 1);
    outlen = len;
    free(b);
    return rc;
}

// //////////////////////////////////////////////////////////////////
// 对外接口
// //////////////////////////////////////////////////////////////////
PORISM_API const char *PORISM_CALL porism_error(void) { return errbuf; }
PORISM_API int PORISM_CALL porism_error_line(void) { return err_line; }
PORISM_API int PORISM_CALL porism_error_col(void) { return err_col; }
PORISM_API size_t PORISM_CALL porism_output_len(void) { return outlen; }
PORISM_API int PORISM_CALL porism_truncated(void) { return out_overflow; }


static void begin_call(char *out, size_t cap) {
    outbuf = out; outcap = cap; outlen = 0; out_overflow = 0;
    json_mode = 0; noutputs = 0; output_cap = 0; outputs = 0;
    if (out && cap) out[0] = 0;
    errbuf[0] = 0; snprintf(err_type, sizeof(err_type), "code");
    err_line = err_col = 0;
}

PORISM_API int PORISM_CALL porism_run(const char *src, char *out, size_t cap) {
    int rc;
    PORISM_LOCK();
    if (!src) {
        begin_call(out, cap);
        snprintf(err_type, sizeof(err_type), "kernel");
        snprintf(errbuf, sizeof(errbuf), "null source");
        rc = 1;
    } else {
        begin_call(out, cap);
        if (setjmp(errjmp)) rc = 1;
        else { run(src, 0); rc = out_overflow ? 2 : 0; }
    }
    PORISM_UNLOCK();
    return rc;
}

PORISM_API int PORISM_CALL porism_eval(const char *expr, const char **names, const double *vals,
                   int nvars, char *out, size_t cap) {
    int rc;
    PORISM_LOCK();
    if (!expr || nvars < 0 || (nvars > 0 && (!names || !vals)) || nvars > MAX_GVARS) {
        begin_call(out, cap);
        snprintf(errbuf, sizeof(errbuf), "invalid arguments to porism_eval");
        rc = 1;
    } else {
        begin_call(out, cap);
        if (setjmp(errjmp)) rc = 1;
        else {
            prepare(strlen(expr), names, vals, nvars);
            Lex l = lex(expr);
            P p; p.t = l.toks; p.i = 0; p.n = l.nt;
            Node *e = parse_expr(&p);
            Tok *rest = pk(&p, 0);
            if (rest->t != T_EOF && rest->t != T_NL)
                fail(rest->line, rest->col, E_TRAILING, got(&p));
            emit(fmtval(eval(e, 0, 0)));
            rc = out_overflow ? 2 : 0;
        }
    }
    PORISM_UNLOCK();
    return rc;
}

PORISM_API const char *PORISM_CALL porism_error_type(void) { return err_type; }

PORISM_API int PORISM_CALL porism_run_json(const char *src, const char *params_json, char *out, size_t cap) {
    int rc;
    PORISM_LOCK();
    begin_call(out, cap);
    json_mode = 1;
    if (!src) {
        snprintf(err_type, sizeof(err_type), "kernel");
        snprintf(errbuf, sizeof(errbuf), "null source");
        rc = write_result_json(1);
    } else if (setjmp(errjmp)) {
        rc = write_result_json(1);
    } else {
        run(src, params_json);
        rc = write_result_json(0);
    }
    json_mode = 0;
    PORISM_UNLOCK();
    return rc;
}

PORISM_API const char *PORISM_CALL porism_abi_version(void) { return PORISM_ABI_VERSION; }

#ifndef PORISM_LIB
static char *readfile(const char *path, const char *what) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "porism: cannot open %s '%s'\n", what, path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "porism: failed to read %s '%s'\n", what, path); exit(2);
    }
    b[n] = 0; fclose(f);
    return b;
}
int main(int argc, char **argv) {
    const char *file = 0, *code = 0, *params = 0, *params_file = 0;
    char *params_owned = 0;   /* -P 读入的参数文件内容，需释放 */
    int json_output = 0;   /* 默认输出 outputs 内容；--json 时输出完整结果对象 */
    for (int i = 1; i < argc; i++) {
        // Version of Interpreter.
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("Porism Interpreter v%s\n", PORISM_VERSION);
            return 0;
        }
        // Help Information.
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Porism Interpreter v%s\n\n", PORISM_VERSION);
            printf("Usage:\n"
                "  porism \"<code>\"                     Run code; print output lines.\n"
                "  porism \"<code>\" -p \"<json>\"        Run code with flat JSON params.\n"
                "  porism \"<code>\" -P <params.json>   Run code with params from a file.\n"
                "  porism -f <file.prsm>               Run a .prsm source file.\n"
                "  porism -f <file.prsm> -p \"<json>\"  Run a file with params.\n"
                "  porism \"<code>\" --json             Print the full JSON result object.\n"
                "  porism -h | --help                  Show this help.\n"
                "  porism -v | --version               Show version.\n"
                "\n"
                "Options:\n"
                "  -f <file.prsm>     Source file to execute.\n"
                "  -p \"<json>\"       Flat JSON object as a string (mutually exclusive with -P).\n"
                "  -P <params.json>   Read the flat JSON object from a file.\n"
                "  --json, -j         Print the full JSON result object instead of\n"
                "                     the output lines (default).\n"
                "  -h, --help         Show this help and exit.\n"
                "  -v, --version      Show version and exit.\n"
                "\n"
                "By default output lines go to stdout; diagnostics go to stderr.\n"
                "Windows CMD: use DOUBLE quotes. Single quotes reach the program\n"
                "literally, so the whole argument is read as a string literal.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--json") || !strcmp(argv[i], "-j")) { json_output = 1; continue; }
        if (!strcmp(argv[i], "-p")) {
            if (i + 1 >= argc) { fprintf(stderr, "porism: -p requires a JSON argument\n"); return 2; }
            params = argv[++i]; continue;
        }
        if (!strcmp(argv[i], "-P")) {
            if (i + 1 >= argc) { fprintf(stderr, "porism: -P requires a JSON file path\n"); return 2; }
            params_file = argv[++i]; continue;
        }
        if (!strcmp(argv[i], "-f")) {
            if (i + 1 >= argc) { fprintf(stderr, "porism: -f requires a file path\n"); return 2; }
            file = argv[++i]; continue;
        }
        /* 形如 -x / --xxx 的未知选项不得静默当作源码 */
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "porism: unknown option '%s'\n", argv[i]);
            return 2;
        }
        if (!code && !file) code = argv[i];
    }
    if (!code && !file) {
        fprintf(stderr, "usage: porism <code> | -f <file.prsm> (-p \"<flat JSON object>\" | -P <params.json>)\n");
        return 2;
    }
    if (params && params_file) {
        fprintf(stderr, "porism: -p and -P are mutually exclusive\n");
        return 2;
    }
    char *src;
    if (file) src = readfile(file, "source file");
    else src = (char *)code;
    if (params_file) {
        params_owned = readfile(params_file, "parameter file");
        params = params_owned;
    }
    size_t cap = 1u << 20;
    char *buf = 0;
    int rc = 2;
    /* 结果对象超出缓冲时按 4 倍重试。求值无副作用，重跑是安全的。 */
    for (int attempt = 0; attempt < 6; attempt++) {
        char *tmp = (char *)realloc(buf, cap);
        if (!tmp) {
            fprintf(stderr, "porism: out of memory (result buffer)\n");
            free(buf); free(params_owned); if (file) free(src); return 1;
        }
        buf = tmp;
        rc = porism_run_json(src, params, buf, cap);
        if (rc != 2) break;
        if (cap > ((size_t)1 << 30)) break;
        cap *= 4;
    }
    if (rc == 2) fprintf(stderr, "porism: result object exceeds %zu bytes\n", cap);
    if (json_output) {
        if (buf[0]) {
            fputs(buf, stdout);
            if (buf[strlen(buf) - 1] != '\n') fputc('\n', stdout);
        }
    } else {
        /* 默认模式：stdout 仅输出 outputs 内容；诊断写 stderr，不混入输出。 */
        for (int i = 0; i < noutputs; i++) {
            fputs(outputs[i], stdout);
            fputc('\n', stdout);
        }
        if (errbuf[0] && rc != 2) {
            fprintf(stderr, "porism: %s at %d:%d: %s\n",
                    err_type, err_line, err_col, errbuf);
        }
    }
    if (file) free(src);
    free(params_owned);
    free(buf);
    return rc == 0 ? 0 : 1;
}
#endif
