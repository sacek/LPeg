/*
** $Id: lpvm.c,v 1.6 2015/09/28 17:01:25 roberto Exp $
** Copyright 2007, Lua.org & PUC-Rio  (see 'lpeg.html' for license)
*/

#include <limits.h>
#include <string.h>


#include "lua.h"
#include "lauxlib.h"

#include "lpcap.h"
#include "lptypes.h"
#include "lpvm.h"
#include "lpprint.h"


/* initial size for call/backtrack stack */
#if !defined(INITBACK)
#define INITBACK	MAXBACK
#endif

#define LRFAIL	-1

#define getoffset(p)	(((p) + 1)->offset)

static const Instruction giveup = {{IGiveup, 0, 0}};


/*
** Decode one UTF-8 sequence, returning NULL if byte sequence is invalid.
*/
static const char *utf8_decode (const char *o, int *val) {
  static const unsigned int limits[] = {0xFF, 0x7F, 0x7FF, 0xFFFFu};
  const unsigned char *s = (const unsigned char *)o;
  unsigned int c = s[0];  /* first byte */
  unsigned int res = 0;  /* final result */
  if (c < 0x80)  /* ascii? */
    res = c;
  else {
    int count = 0;  /* to count number of continuation bytes */
    while (c & 0x40) {  /* still have continuation bytes? */
      int cc = s[++count];  /* read next byte */
      if ((cc & 0xC0) != 0x80)  /* not a continuation byte? */
        return NULL;  /* invalid byte sequence */
      res = (res << 6) | (cc & 0x3F);  /* add lower 6 bits from cont. byte */
      c <<= 1;  /* to test next bit */
    }
    res |= (c & 0x7F) << (count * 5);  /* add first byte */
    if (count > 3 || res > 0x10FFFFu || res <= limits[count])
      return NULL;  /* invalid byte sequence */
    s += count;  /* skip continuation bytes read */
  }
  *val = res;
  return (const char *)s + 1;  /* +1 to include first byte */
}


/*
** {======================================================
** Virtual Machine
** =======================================================
*/


typedef struct Stack {
  const char *s;  /* saved position (or NULL for calls) */
  const Instruction *p;  /* next instruction */
  int caplevel;
  const char *X; /* LR */
  const Instruction *pA;
} Stack;


#define getstackbase(L, ptop)	((Stack *)lua_touserdata(L, stackidx(ptop)))


/*
** Ensures the size of array 'capture' (with size '*capsize' and
** 'captop' elements being used) is enough to accomodate 'n' extra
** elements plus one.  (Because several opcodes add stuff to the capture
** array, it is simpler to ensure the array always has at least one free
** slot upfront and check its size later.)
*/

/* new size in number of elements cannot overflow integers, and new
   size in bytes cannot overflow size_t. */
#define MAXNEWSIZE  \
    (((size_t)INT_MAX) <= (~(size_t)0 / sizeof(Capture)) ?  \
     ((size_t)INT_MAX) : (~(size_t)0 / sizeof(Capture)))

static Capture *growcap (lua_State *L, Capture *capture, int *capsize,
                                       int captop, int n, int ptop) {
  if (*capsize - captop > n)
    return capture;  /* no need to grow array */
  else {  /* must grow */
    Capture *newc;
    unsigned int newsize = captop + n + 1;  /* minimum size needed */
    if (newsize < MAXNEWSIZE / 2)
      newsize *= 2;  /* twice that size, if not too big */
    else if (newsize < (MAXNEWSIZE / 9) * 8)
      newsize += newsize / 8;  /* else, try 9/8 that size */
    else
      luaL_error(L, "too many captures");
    newc = (Capture *)lua_newuserdata(L, newsize * sizeof(Capture));
    memcpy(newc, capture, captop * sizeof(Capture));
    *capsize = newsize;
    lua_replace(L, caplistidx(ptop));
    return newc;
  }
}

/*
** Double the size of the Stack of captures
*/
static CaptureStack *doublecapstack (lua_State *L, int capstacktop, int ptop) {
  CaptureStack *newcs;
  CaptureStack *capstack = ((CaptureStack *)lua_touserdata(L, capliststackidx(ptop)));
  if (capstacktop >= INT_MAX/((int)sizeof(CaptureStack) * 2))
    luaL_error(L, "too many captures lists");
  newcs = (CaptureStack *)lua_newuserdata(L, capstacktop * 2 * sizeof(CaptureStack));
  memcpy(newcs, capstack, capstacktop * sizeof(CaptureStack));
  lua_replace(L, capliststackidx(ptop));
  return newcs;
}

/*
** new capture
*/
static Capture *newcap (lua_State *L, int size, int ptop, int capstackptr) {
  Capture *newc = (Capture *)lua_newuserdata(L, size * sizeof(Capture));
  lua_replace(L, caplistidx(ptop));
  lua_pushvalue(L, caplistidx(ptop)); // update capture base in Capture Stack
  lua_rawseti(L, caplistsidx(ptop), capstackptr);
  return newc;
}

/*
** Double the size of the stack
*/
static Stack *doublestack (lua_State *L, Stack **stacklimit, int ptop) {
  Stack *stack = getstackbase(L, ptop);
  Stack *newstack;
  int n = *stacklimit - stack;  /* current stack size */
  int max, newn;
  lua_getfield(L, LUA_REGISTRYINDEX, MAXSTACKIDX);
  max = lua_tointeger(L, -1);  /* maximum allowed size */
  lua_pop(L, 1);
  if (n >= max)  /* already at maximum size? */
    luaL_error(L, "backtrack stack overflow (current limit is %d)", max);
  newn = 2 * n;  /* new size */
  if (newn > max) newn = max;
  newstack = (Stack *)lua_newuserdata(L, newn * sizeof(Stack));
  memcpy(newstack, stack, n * sizeof(Stack));
  lua_replace(L, stackidx(ptop));
  *stacklimit = newstack + newn;
  return newstack + n;  /* return next position */
}


/*
** Interpret the result of a dynamic capture: false -> fail;
** true -> keep current position; number -> next position.
** Return new subject position. 'fr' is stack index where
** is the result; 'curr' is current subject position; 'limit'
** is subject's size.
*/
static int resdyncaptures (lua_State *L, int fr, int curr, int limit) {
  lua_Integer res;
  if (!lua_toboolean(L, fr)) {  /* false value? */
    lua_settop(L, fr - 1);  /* remove results */
    return -1;  /* and fail */
  }
  else if (lua_isboolean(L, fr))  /* true? */
    res = curr;  /* keep current position */
  else {
    res = lua_tointeger(L, fr) - 1;  /* new position */
    if (res < curr || res > limit)
      luaL_error(L, "invalid position returned by match-time capture");
  }
  lua_remove(L, fr);  /* remove first result (offset) */
  return res;
}


/*
** Add capture values returned by a dynamic capture to the list
** 'capture', nested inside a group. 'fd' indexes the first capture
** value, 'n' is the number of values (at least 1). The open group
** capture is already in 'capture', before the place for the new entries.
*/
static void adddyncaptures (const char *s, Capture *capture, int n, int fd) {
  int i;
  assert(capture[-1].kind == Cgroup && capture[-1].siz == 0);
  capture[-1].idx = 0;  /* make group capture an anonymous group */
  for (i = 0; i < n; i++) {  /* add runtime captures */
    capture[i].kind = Cruntime;
    capture[i].siz = 1;  /* mark it as closed */
    capture[i].idx = fd + i;  /* stack index of capture value */
    capture[i].s = s;
  }
  capture[n].kind = Cclose;  /* close group */
  capture[n].siz = 1;
  capture[n].s = s;
}


/*
** Remove dynamic captures from the Lua stack (called in case of failure)
*/
static int removedyncap (lua_State *L, Capture *capture,
                         int level, int last) {
  int id = finddyncap(capture + level, capture + last);  /* index of 1st cap. */
  int top = lua_gettop(L);
  if (id == 0) return 0;  /* no dynamic captures? */
  lua_settop(L, id - 1);  /* remove captures */
  return top - id + 1;  /* number of values removed */
}

/*
**
*/
static Capture* getcapturesfromstack (lua_State *L, int ndyncap, int newdyncap, int capstacktop, int ptop) {
  int i;
  Capture * capture;
  lua_rawgeti(L,caplistsidx(ptop), capstacktop);
  capture = ((Capture *)lua_touserdata(L, -1));
  lua_replace(L, caplistidx(ptop));
  lua_pop(L,ndyncap);
  lua_rawgeti(L,dyncaplistidx(ptop), capstacktop);
  for (i = 1; i <= newdyncap; i++)
   {
    lua_rawgeti(L, -1, i);
    lua_insert(L,-2);
   }
  lua_pop(L,1);
  return capture;
}

/*
**
*/
static CaptureStack * addcapturestostack (lua_State *L, CaptureStack *capstack, int ndyncap, int captop, int *capstacksize, int *capstacktop, int ptop) {
   int i;
   capstack->captop = captop;
   capstack->dyncaptop = ndyncap;
   lua_newtable(L);
   for (i = 1; i <= ndyncap; i++)
    {
     lua_pushvalue(L,i - ndyncap - 2);
     lua_rawseti(L, -2, i);
    }
    lua_rawseti(L, dyncaplistidx(ptop), *capstacktop);
    if (*capstacktop + 1 >= *capstacksize) {
      capstack = doublecapstack(L, *capstacktop,  ptop) + *capstacktop - 1;
      *capstacksize = 2 * *capstacktop;
    }
    (*capstacktop)++;
    capstack++;
    return capstack;
}



/*
**
*/
static int removecapturesfromstack (lua_State *L, int capstacktop, int ptop) {
  lua_pushnil(L);
  lua_rawseti(L,caplistsidx(ptop),capstacktop);
  lua_pushnil(L);
  lua_rawseti(L,dyncaplistidx(ptop),capstacktop);
  return --capstacktop;
}

/*
**
*/
static void putcapturestolambda (lua_State *L, int ndyncap, int captop, int ptop) {
  int i;
  lua_pushvalue(L, caplistidx(ptop));
  lua_setfield(L,-2,"commitcap");
  lua_pushinteger(L, captop);
  lua_setfield(L,-2,"commitcaptop");
  lua_newtable(L);
  for (i = 1; i <= ndyncap; i++)
    {
     lua_pushvalue(L,i - ndyncap - 1 - 2);
     lua_rawseti(L,-2,i);
    }
  lua_pushinteger(L, ndyncap);
  lua_setfield(L,-3,"commitdyncapcount");
  lua_setfield(L,-2,"commitdyncap");
}

/*
**
*/
static Capture * addcapturesfromlambda (lua_State *L, int lambdaindex, Capture * capture,  int *ndyncap, int *captop, int *capsize, int ptop) {
  int i, commitdyncapcount, commitcaptop;
  Capture * commitcapture;
  lua_pushinteger(L, lambdaindex);
  lua_gettable(L, lambdaidx(ptop));
  lua_getfield(L,-1,"commitcap");
  commitcapture = ((Capture *)lua_touserdata(L, -1));
  lua_getfield(L,-2,"commitcaptop");
  commitcaptop = lua_tointeger(L, -1);
  lua_getfield(L,-3,"commitdyncapcount");
  commitdyncapcount = lua_tointeger(L, -1);
  lua_getfield(L,-4,"commitdyncap");
  for (i = 1; i <= commitdyncapcount; i++)
   {
    lua_rawgeti(L, -1, i);
    lua_insert(L,-6);
   }
  lua_pop(L,5);
  for (i = 0; i < commitcaptop; i++)
    if (commitcapture[i].kind == Cruntime)
      commitcapture[i].idx += *ndyncap;
  *ndyncap += commitdyncapcount;
  if (commitcaptop > 0) {
    if (*captop + commitcaptop >= *capsize) {
      capture = growcap(L, capture, capsize, *captop + commitcaptop, 0, ptop);
    }
    memcpy(capture + *captop, commitcapture, commitcaptop * sizeof(Capture));
    *captop += commitcaptop;
  }
  return capture;
}


/*
**
*/
static void clearlambdaitem (lua_State *L, int index, int ptop) {
  lua_pushinteger(L, index);
  lua_pushnil(L);
  lua_settable(L, lambdaidx(ptop));
}

/*
** Opcode interpreter
*/
const char *match (lua_State *L, const char *o, const char *s, const char *e,
                   Instruction *op, Capture *capture, int ptop) {
  Stack stackbase[INITBACK];
  Stack *stacklimit = stackbase + INITBACK;
  Stack *stack = stackbase;  /* point to first empty slot in stack */
  int capsize = INITCAPSIZE;
  int captop = 0;  /* point to first empty slot in captures */
  int ndyncap = 0;  /* number of dynamic captures (in Lua stack) */
  const Instruction *p = op;  /* current instruction */
  int maxpointer = e - o;
  CaptureStack capstackbase[INITCAPSTACKSIZE];
  CaptureStack *capstack = capstackbase;
  int capstacksize = INITCAPSTACKSIZE;
  int capstacktop = 0;
  stack->X = NULL;
  stack->p = &giveup; stack->s = s; stack->caplevel = 0; stack++;
  lua_pushlightuserdata(L, stackbase);
  lua_newtable(L); // Lambda (L for left recursion) Lua stack index lambdaidx
  lua_newtable(L); // Captures Lists  (Captures for left recursion) Lua stack index caplistsidx
  lua_pushlightuserdata(L, capstackbase); //capliststackidx(ptop)
  lua_newtable(L); // Dynamic capture list dyncaplistidx(ptop)
  capstacktop++;
  lua_pushvalue(L, caplistidx(ptop)); // set Capture list base to first slot of Captures List array
  lua_rawseti(L,caplistsidx(ptop),capstacktop);
  capstack->captop = captop;
  capstack->dyncaptop = ndyncap;
  capstack->capsize = capsize;
  for (;;) {
#if defined(DEBUG)
      printf("-------------------------------------\n");
      printcaplist(capture, capture + captop);
      printf("s: |%s| stck:%d, dyncaps:%d, caps:%d  ",
             s, (int)(stack - getstackbase(L, ptop)), ndyncap, captop);
      printinst(op, p);
#endif
    assert(dyncaplistidx(ptop) + ndyncap == lua_gettop(L) && ndyncap <= captop);
    switch ((Opcode)p->i.code) {
      case IEnd: {
        assert(stack == getstackbase(L, ptop) + 1);
        capture[captop].kind = Cclose;
        capture[captop].s = NULL;
        return s;
      }
      case IGiveup: {
        assert(stack == getstackbase(L, ptop));
        return NULL;
      }
      case IRet: {
        if (!(stack - 1)->X) { // not LR return
          assert(stack > getstackbase(L, ptop) && (stack - 1)->s == NULL);
          p = (--stack)->p;
        }
        else
        {
         const char* X = (stack - 1)->X;
         if (X == (char*)LRFAIL || s > X) { // rule lvar.1 inc.1
           (stack - 1)->X = s;
            p = (stack - 1)->pA;
            s = (stack - 1)->s;
            (stack - 1)->caplevel = captop;
            lua_pushinteger(L, (p - op) * maxpointer + (s - o));
            lua_gettable(L, lambdaidx(ptop));
            lua_pushinteger(L,(stack - 1)->X == (char*)LRFAIL ? LRFAIL : (stack - 1)->X - o);
            lua_setfield(L,-2,"X");
            putcapturestolambda (L, ndyncap, captop, ptop);
            lua_pop(L,1);
            if (ndyncap > 0)
              lua_pop(L, ndyncap);
            ndyncap = 0;
            captop = 0;
            capsize = INITCAPSIZE;
            capture = newcap (L, capsize, ptop, capstacktop);
            capstack->captop = captop;
            capstack->capsize = capsize;
            capstack->dyncaptop = ndyncap;
        }
         else {  // rule inc.3
           int newdyncap, lambdaindex;
           stack--;
           p = stack->p;
           s = stack->X;
           capstacktop = removecapturesfromstack (L, capstacktop, ptop);
           capstack--;
           captop = capstack->captop;
           capsize = capstack->capsize;
           newdyncap = capstack->dyncaptop;
           capture = getcapturesfromstack (L, ndyncap, newdyncap, capstacktop, ptop);
           ndyncap = newdyncap;
           lambdaindex = (stack->pA - op) * maxpointer + (stack->s - o);
           capture = addcapturesfromlambda (L, lambdaindex, capture, &ndyncap, &captop, &capsize, ptop);
           clearlambdaitem (L, lambdaindex, ptop);
         }
        }
        continue;
      }
      case IAny: {
        if (s < e) { p++; s++; }
        else goto fail;
        continue;
      }
      case IUTFR: {
        int codepoint;
        if (s >= e)
          goto fail;
        s = utf8_decode (s, &codepoint);
        if (s && p[1].offset <= codepoint && codepoint <= utf_to(p))
          p += 2;
        else
          goto fail;
        continue;
      }
      case ITestAny: {
        if (s < e) p += 2;
        else p += getoffset(p);
        continue;
      }
      case IChar: {
        if ((byte)*s == p->i.aux && s < e) { p++; s++; }
        else goto fail;
        continue;
      }
      case ITestChar: {
        if ((byte)*s == p->i.aux && s < e) p += 2;
        else p += getoffset(p);
        continue;
      }
      case ISet: {
        int c = (byte)*s;
        if (testchar((p+1)->buff, c) && s < e)
          { p += CHARSETINSTSIZE; s++; }
        else goto fail;
        continue;
      }
      case ITestSet: {
        int c = (byte)*s;
        if (testchar((p + 2)->buff, c) && s < e)
          p += 1 + CHARSETINSTSIZE;
        else p += getoffset(p);
        continue;
      }
      case IBehind: {
        int n = p->i.aux;
        if (n > s - o) goto fail;
        s -= n; p++;
        continue;
      }
      case ISpan: {
        for (; s < e; s++) {
          int c = (byte)*s;
          if (!testchar((p+1)->buff, c)) break;
        }
        p += CHARSETINSTSIZE;
        continue;
      }
      case IJmp: {
        p += getoffset(p);
        continue;
      }
      case IChoice: {
        if (stack == stacklimit)
          stack = doublestack(L, &stacklimit, ptop);
        stack->p = p + getoffset(p);
        stack->s = s;
        stack->caplevel = captop;
        stack->X = NULL;
        stack++;
        p += 2;
        continue;
      }
      case ICall: {
        int k = p->i.aux;
        if (stack == stacklimit)
          stack = doublestack(L, &stacklimit, ptop);
        if (k == 0) { // not LR call
          stack->s = NULL;
          stack->X = NULL;
          stack->p = p + 2;  /* save return address */
          stack++;
          p += getoffset(p);
        }
        else
        {
         const Instruction *pA = p + getoffset(p);
         int lambdaindex = (pA - op) * maxpointer + (s - o);
         lua_pushinteger(L, lambdaindex);
         lua_gettable(L, lambdaidx(ptop));
         if (!lua_istable(L,-1)) {  // rule lvar.1 lvar.2
           lua_pushinteger(L, lambdaindex);
           lua_newtable(L);
           lua_pushinteger(L, LRFAIL);
           lua_setfield(L,-2,"X");
           lua_pushinteger(L, k);
           lua_setfield(L,-2,"k");
           lua_settable(L, lambdaidx(ptop));
           lua_pop(L, 1);
           capstack = addcapturestostack(L, capstack, ndyncap, captop, &capstacksize, &capstacktop, ptop);
           if (ndyncap > 0)
             lua_pop(L, ndyncap);
           ndyncap = 0;
           captop = 0;
           capsize = INITCAPSIZE;
           capture = newcap (L, capsize, ptop, capstacktop);
           capstack->captop = captop;
           capstack->dyncaptop = ndyncap;
           capstack->capsize = capsize;
           stack->p = p + 2;
           stack->pA = pA;
           stack->s = s;
           stack->X = (char*)LRFAIL;
           stack->caplevel = captop;
           stack++;
           p += getoffset(p);
          }
          else
          {
           int X_X, X_k;
           lua_getfield(L, -1, "X");
           X_X = lua_tointeger(L,-1);
           lua_getfield(L, -2, "k");
           X_k = lua_tointeger(L,-1);
           lua_pop(L, 3);
           if (X_X == LRFAIL || k < X_k) // rule lvar.3 lvar.5
            goto fail;
           else // rule  lvar.4
            {
             capture = addcapturesfromlambda (L, lambdaindex, capture, &ndyncap, &captop, &capsize, ptop);
             p += 2;
             s = o + X_X;
            }
          }
        }
        continue;
      }
      case ICommit: {
        assert(stack > getstackbase(L, ptop) && (stack - 1)->s != NULL);
        stack--;
        p += getoffset(p);
        continue;
      }
      case IPartialCommit: {
        assert(stack > getstackbase(L, ptop) && (stack - 1)->s != NULL);
        (stack - 1)->s = s;
        (stack - 1)->caplevel = captop;
        p += getoffset(p);
        continue;
      }
      case IBackCommit: {
        assert(stack > getstackbase(L, ptop) && (stack - 1)->s != NULL);
        s = (--stack)->s;
        captop = stack->caplevel;
        p += getoffset(p);
        continue;
      }
      case IFailTwice:
        assert(stack > getstackbase(L, ptop));
        stack--;
        /* FALLTHROUGH */
      case IFail:
      fail: { /* pattern failed: try to backtrack */
        const char* X;
        int newdyncap;
        do {  /* remove pending calls */
          assert(stack > getstackbase(L, ptop));
          s = (--stack)->s;
          X = stack->X;
          if (X == (char*)LRFAIL) { // rule lvar.2 rest
            capstacktop = removecapturesfromstack (L, capstacktop, ptop);
            capstack--;
            captop = capstack->captop;
            capsize = capstack->capsize;
            newdyncap = capstack->dyncaptop;
            capture = getcapturesfromstack (L, ndyncap, newdyncap, capstacktop, ptop);
            ndyncap = newdyncap;
            clearlambdaitem (L, (stack->pA - op) * maxpointer + (s - o), ptop);
          }
        } while (s == NULL || X == (char*)LRFAIL);

        if (ndyncap > 0)  /* is there matchtime captures? */
          ndyncap -= removedyncap(L, capture, stack->caplevel, captop);
        p = stack->p;
#if defined(DEBUG)
        printf("**FAIL**\n");
#endif
        if (X) // rule inc.2
        {
         int lambdaindex;
         s = X;
         capstacktop = removecapturesfromstack (L, capstacktop, ptop);
         capstack--;
         captop = capstack->captop;
         capsize = capstack->capsize;
         newdyncap = capstack->dyncaptop;
         capture = getcapturesfromstack (L, ndyncap, newdyncap, capstacktop, ptop);
         ndyncap = newdyncap;
         lambdaindex = (stack->pA - op) * maxpointer + (stack->s - o);
         capture = addcapturesfromlambda (L, lambdaindex, capture, &ndyncap, &captop, &capsize, ptop);
         clearlambdaitem (L, lambdaindex, ptop);
        }
        else
          captop = stack->caplevel;
        continue;
      }
      case ICloseRunTime: {
        CapState cs;
        int rem, res, n;
        int fr = lua_gettop(L) + 1;  /* stack index of first result */
        cs.reclevel = 0; cs.L = L;
        cs.s = o; cs.ocap = capture; cs.ptop = ptop;
        n = runtimecap(&cs, capture + captop, s, &rem);  /* call function */
        captop -= n;  /* remove nested captures */
        ndyncap -= rem;  /* update number of dynamic captures */
        fr -= rem;  /* 'rem' items were popped from Lua stack */
        res = resdyncaptures(L, fr, s - o, e - o);  /* get result */
        if (res == -1)  /* fail? */
          goto fail;
        s = o + res;  /* else update current position */
        n = lua_gettop(L) - fr + 1;  /* number of new captures */
        ndyncap += n;  /* update number of dynamic captures */
        if (n == 0)  /* no new captures? */
          captop--;  /* remove open group */
        else {  /* new captures; keep original open group */
          if (fr + n >= SHRT_MAX)
            luaL_error(L, "too many results in match-time capture");
          /* add new captures + close group to 'capture' list */
          capture = growcap(L, capture, &capsize, captop, n + 1, ptop);
          adddyncaptures(s, capture + captop, n, fr);
          captop += n + 1;  /* new captures + close group */
        }
        p++;
        continue;
      }
      case ICloseCapture: {
        const char *s1 = s;
        assert(captop > 0);
        /* if possible, turn capture into a full capture */
        if (capture[captop - 1].siz == 0 &&
            s1 - capture[captop - 1].s < UCHAR_MAX) {
          capture[captop - 1].siz = s1 - capture[captop - 1].s + 1;
          p++;
          continue;
        }
        else {
          capture[captop].siz = 1;  /* mark entry as closed */
          capture[captop].s = s;
          goto pushcapture;
        }
      }
      case IOpenCapture:
        capture[captop].siz = 0;  /* mark entry as open */
        capture[captop].s = s;
        goto pushcapture;
      case IFullCapture:
        capture[captop].siz = getoff(p) + 1;  /* save capture size */
        capture[captop].s = s - getoff(p);
        /* goto pushcapture; */
      pushcapture: {
        capture[captop].idx = p->i.key;
        capture[captop].kind = getkind(p);
        captop++;
        capture = growcap(L, capture, &capsize, captop, 0, ptop);
        p++;
        continue;
      }
      default: assert(0); return NULL;
    }
  }
}

/* }====================================================== */


