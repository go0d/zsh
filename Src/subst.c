/*
 * subst.c - various substitutions
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1992-1997 Paul Falstad
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Paul Falstad or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Paul Falstad and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Paul Falstad and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Paul Falstad and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "zsh.mdh"
#include "subst.pro"

#define LF_ARRAY	1

/**/
char nulstring[] = {Nularg, '\0'};

/* Do substitutions before fork. These are:
 *  - Process substitution: <(...), >(...), =(...)
 *  - Parameter substitution
 *  - Command substitution
 * Followed by
 *  - Quote removal
 *  - Brace expansion
 *  - Tilde and equals substitution
 *
 * PF_* flags are defined in zsh.h
 */

/**/
mod_export void
prefork(LinkList list, int flags)
{
    LinkNode node, stop = 0;
    int keep = 0, asssub = (flags & PF_TYPESET) && isset(KSHTYPESET);

    queue_signals();
    for (node = firstnode(list); node; incnode(node)) {
	if (isset(SHFILEEXPANSION)) {
	    /*
	     * Here and below we avoid taking the address
	     * of a void * and then pretending it's a char **
	     * instead of a void ** by a little inefficiency.
	     * This could be avoided with some extra linked list
	     * machinery, but that would need quite a lot of work
	     * to ensure consistency.  What we really need is
	     * templates...
	     */
	    char *cptr = (char *)getdata(node);
	    filesub(&cptr, flags & (PF_TYPESET|PF_ASSIGN));
	    /*
	     * The assignment is so simple it's not worth
	     * testing if cptr changed...
	     */
	    setdata(node, cptr);
	}
	if (!(node = stringsubst(list, node, flags & PF_SINGLE, asssub))) {
	    unqueue_signals();
	    return;
	}
    }
    for (node = firstnode(list); node; incnode(node)) {
	if (node == stop)
	    keep = 0;
	if (*(char *)getdata(node)) {
	    remnulargs(getdata(node));
	    if (unset(IGNOREBRACES) && !(flags & PF_SINGLE)) {
		if (!keep)
		    stop = nextnode(node);
		while (hasbraces(getdata(node))) {
		    keep = 1;
		    xpandbraces(list, &node);
		}
	    }
	    if (unset(SHFILEEXPANSION)) {
		char *cptr = (char *)getdata(node);
		filesub(&cptr, flags & (PF_TYPESET|PF_ASSIGN));
		setdata(node, cptr);
	    }
	} else if (!(flags & PF_SINGLE) && !keep)
	    uremnode(list, node);
	if (errflag) {
	    unqueue_signals();
	    return;
	}
    }
    unqueue_signals();
}

/*
 * Perform $'...' quoting.  The arguments are
 *   strstart   The start of the string
 *   pstrdpos   Initially, *pstrdpos is the position where the $ of the $'
 *              occurs.  It will be updated to the next character after the
 *              last ' of the $'...'.
 * The return value is the entire allocated string from strstart on the heap.
 * Note the original string may be modified in the process.
 */
/**/
static char *
stringsubstquote(char *strstart, char **pstrdpos)
{
    int len;
    char *strdpos = *pstrdpos, *strsub, *strret;

    strsub = getkeystring(strdpos+2, &len,
			  GETKEYS_DOLLARS_QUOTE, NULL);
    len += 2;			/* measured from strdpos */

    if (strstart != strdpos) {
	*strdpos = '\0';
	if (strdpos[len])
	    strret = zhtricat(strstart, strsub, strdpos + len);
	else
	    strret = dyncat(strstart, strsub);
    } else if (strdpos[len])
	strret = dyncat(strsub, strdpos + len);
    else
	strret = strsub;

    *pstrdpos = strret + (strdpos - strstart) + strlen(strsub);

    return strret;
}

/**/
static LinkNode
stringsubst(LinkList list, LinkNode node, int ssub, int asssub)
{
    int qt;
    char *str3 = (char *)getdata(node);
    char *str  = str3, c;

    while (!errflag && (c = *str)) {
	if (((c = *str) == Inang || c == OutangProc ||
	     (str == str3 && c == Equals))
	    && str[1] == Inpar) {
	    char *subst, *rest, *snew, *sptr;
	    int str3len = str - str3, sublen, restlen;

	    if (c == Inang || c == OutangProc)
		subst = getproc(str, &rest);	/* <(...) or >(...) */
	    else
		subst = getoutputfile(str, &rest);	/* =(...) */
	    if (!subst)
		subst = "";

	    sublen = strlen(subst);
	    restlen = strlen(rest);
	    sptr = snew = hcalloc(str3len + sublen + restlen + 1);
	    if (str3len) {
		memcpy(sptr, str3, str3len);
		sptr += str3len;
	    }
	    if (sublen) {
		memcpy(sptr, subst, sublen);
		sptr += sublen;
	    }
	    if (restlen)
		memcpy(sptr, rest, restlen);
	    sptr[restlen] = '\0';
	    str3 = snew;
	    str = snew + str3len + sublen;
	    setdata(node, str3);
	} else
	    str++;
    }
    str = str3;

    while (!errflag && (c = *str)) {
	if ((qt = c == Qstring) || c == String) {
	    if ((c = str[1]) == Inpar) {
		if (!qt)
		    list->list.flags |= LF_ARRAY;
		str++;
		goto comsub;
	    } else if (c == Inbrack) {
		/* $[...] */
		char *str2 = str;
		str2++;
		if (skipparens(Inbrack, Outbrack, &str2)) {
		    zerr("closing bracket missing");
		    return NULL;
		}
		str2[-1] = *str = '\0';
		str = arithsubst(str + 2, &str3, str2);
		setdata(node, (void *) str3);
		continue;
	    } else if (c == Snull) {
		str3 = stringsubstquote(str3, &str);
		setdata(node, (void *) str3);
		continue;
	    } else {
		node = paramsubst(list, node, &str, qt, ssub);
		if (errflag || !node)
		    return NULL;
		str3 = (char *)getdata(node);
		continue;
	    }
	} else if ((qt = c == Qtick) || (c == Tick ? (list->list.flags |= LF_ARRAY) : 0))
	  comsub: {
	    LinkList pl;
	    char *s, *str2 = str;
	    char endchar;
	    int l1, l2;

	    if (c == Inpar) {
		endchar = Outpar;
		str[-1] = '\0';
#ifdef DEBUG
		if (skipparens(Inpar, Outpar, &str))
		    dputs("BUG: parse error in command substitution");
#else
		skipparens(Inpar, Outpar, &str);
#endif
		str--;
	    } else {
		endchar = c;
		*str = '\0';

		while (*++str != endchar)
		    DPUTS(!*str, "BUG: parse error in command substitution");
	    }
	    *str++ = '\0';
	    if (endchar == Outpar && str2[1] == '(' && str[-2] == ')') {
		/* Math substitution of the form $((...)) */
		str[-2] = '\0';
		str = arithsubst(str2 + 2, &str3, str);
		setdata(node, (void *) str3);
		continue;
	    }

	    /* It is a command substitution, which will be parsed again   *
	     * by the lexer, so we untokenize it first, but we cannot use *
	     * untokenize() since in the case of `...` some Bnulls should *
	     * be left unchanged.  Note that the lexer doesn't tokenize   *
	     * the body of a command substitution so if there are some    *
	     * tokens here they are from a ${(e)~...} substitution.       */
	    for (str = str2; (c = *++str); )
		if (itok(c) && c != Nularg &&
		    !(endchar != Outpar && c == Bnull &&
		      (str[1] == '$' || str[1] == '\\' || str[1] == '`' ||
		       (qt && str[1] == '"'))))
		    *str = ztokens[c - Pound];
	    str++;
	    if (!(pl = getoutput(str2 + 1, qt || ssub))) {
		zerr("parse error in command substitution");
		return NULL;
	    }
	    if (endchar == Outpar)
		str2--;
	    if (!(s = (char *) ugetnode(pl))) {
		str = strcpy(str2, str);
		continue;
	    }
	    if (!qt && ssub && isset(GLOBSUBST))
		shtokenize(s);
	    l1 = str2 - str3;
	    l2 = strlen(s);
	    if (nonempty(pl)) {
		LinkNode n = lastnode(pl);
		str2 = (char *) hcalloc(l1 + l2 + 1);
		strcpy(str2, str3);
		strcpy(str2 + l1, s);
		setdata(node, str2);
		insertlinklist(pl, node, list);
		s = (char *) getdata(node = n);
		l1 = 0;
		l2 = strlen(s);
	    }
	    str2 = (char *) hcalloc(l1 + l2 + strlen(str) + 1);
	    if (l1)
		strcpy(str2, str3);
	    strcpy(str2 + l1, s);
	    str = strcpy(str2 + l1 + l2, str);
	    str3 = str2;
	    setdata(node, str3);
	    continue;
	} else if (asssub && ((c == '=') || c == Equals) && str != str3) {
	    /*
	     * We are in a normal argument which looks like an assignment
	     * and is to be treated like one, with no word splitting.
	     */
	    ssub = 1;
	}
	str++;
    }
    return errflag ? NULL : node;
}

/*
 * Simplified version of the prefork/singsub processing where
 * we only do substitutions appropriate to quoting.  Currently
 * this means only the expansions in $'....'.  This is used
 * for the end tag for here documents.  As we are not doing
 * `...` expansions, we just use those for quoting.  However,
 * they stay in the text.  This is weird, but that's not
 * my fault.
 *
 * The remnulargs() makes this consistent with the other forms
 * of substitution, indicating that quotes have been fully
 * processed.
 *
 * The fully processed string is returned.
 */

/**/
char *
quotesubst(char *str)
{
    char *s = str;

    while (*s) {
	if (*s == String && s[1] == Snull) {
	    str = stringsubstquote(str, &s);
	} else {
	    s++;
	}
    }
    remnulargs(str);
    return str;
}

/**/
mod_export void
globlist(LinkList list, int nountok)
{
    LinkNode node, next;

    badcshglob = 0;
    for (node = firstnode(list); !errflag && node; node = next) {
	next = nextnode(node);
	zglob(list, node, nountok);
    }
    if (badcshglob == 1)
	zerr("no match");
}

/* perform substitution on a single word */

/**/
mod_export void
singsub(char **s)
{
    local_list1(foo);

    init_list1(foo, *s);

    prefork(&foo, PF_SINGLE);
    if (errflag)
	return;
    *s = (char *) ugetnode(&foo);
    DPUTS(nonempty(&foo), "BUG: singsub() produced more than one word!");
}

/* Perform substitution on a single word, *s. Unlike with singsub(), the
 * result can be more than one word. If split is non-zero, the string is
 * first word-split using IFS, but only for non-quoted "whitespace" (as
 * indicated by Dnull, Snull, Tick, Bnull, Inpar, and Outpar).
 *
 * If arg "a" was non-NULL and we got an array as a result of the parsing,
 * the strings are stored in *a (even for a 1-element array) and *isarr is
 * set to 1.  Otherwise, *isarr is set to 0, and the result is put into *s,
 * with any necessary joining of multiple elements using sep (which can be
 * NULL to use IFS).  The return value is true iff the expansion resulted
 * in an empty list. */

/**/
static int
multsub(char **s, int split, char ***a, int *isarr, char *sep)
{
    int l;
    char **r, **p, *x = *s;
    local_list1(foo);

    if (split) {
	/*
	 * This doesn't handle multibyte characters, but we're
	 * looking for whitespace separators which must be ASCII.
	 */
	for ( ; *x; x += l) {
	    char c = (l = *x == Meta) ? x[1] ^ 32 : *x;
	    l++;
	    if (!iwsep(STOUC(c)))
		break;
	}
    }

    init_list1(foo, x);

    if (split) {
	LinkNode n = firstnode(&foo);
	int inq = 0, inp = 0;
	MB_METACHARINIT();
	for ( ; *x; x += l) {
	    int rawc = -1;
	    convchar_t c;
	    if (itok(STOUC(*x))) {
		/* token, can't be separator, must be single byte */
		rawc = *x;
		l = 1;
	    } else {
		l = MB_METACHARLENCONV(x, &c);
		if (!inq && !inp && WC_ZISTYPE(c, ISEP)) {
		    *x = '\0';
		    for (x += l; *x; x += l) {
			if (itok(STOUC(*x))) {
			    /* as above */
			    rawc = *x;
			    l = 1;
			    break;
			}
			l = MB_METACHARLENCONV(x, &c);
			if (!WC_ZISTYPE(c, ISEP))
			    break;
		    }
		    if (!*x)
			break;
		    insertlinknode(&foo, n, (void *)x), incnode(n);
		}
	    }
	    switch (rawc) {
	    case Dnull:  /* " */
	    case Snull:  /* ' */
	    case Tick:   /* ` (note: no Qtick!) */
		/* These always occur in unnested pairs. */
		inq = !inq;
		break;
	    case Inpar:  /* ( */
		inp++;
		break;
	    case Outpar: /* ) */
		inp--;
		break;
	    case Bnull:  /* \ */
	    case Bnullkeep:
		/* The parser verified the following char's existence. */
		x += l;
		l = MB_METACHARLEN(x);
		break;
	    }
	}
    }

    prefork(&foo, 0);
    if (errflag) {
	if (isarr)
	    *isarr = 0;
	return 0;
    }

    if ((l = countlinknodes(&foo)) > 1 || (foo.list.flags & LF_ARRAY && a)) {
	p = r = hcalloc((l + 1) * sizeof(char*));
	while (nonempty(&foo))
	    *p++ = (char *)ugetnode(&foo);
	*p = NULL;
	/* We need a way to figure out if a one-item result was a scalar
	 * or a single-item array.  The parser will have set LF_ARRAY
	 * in the latter case, allowing us to return it as an array to
	 * our caller (if they provided for that result). */
	if (a && (l > 1 || foo.list.flags & LF_ARRAY)) {
	    *a = r;
	    *isarr = SCANPM_MATCHMANY;
	    return 0;
	}
	*s = sepjoin(r, sep, 1);
	if (isarr)
	    *isarr = 0;
	return 0;
    }
    if (l)
	*s = (char *) ugetnode(&foo);
    else
	*s = dupstring("");
    if (isarr)
	*isarr = 0;
    return !l;
}

/*
 * ~, = subs: assign & PF_TYPESET => typeset or magic equals
 *            assign & PF_ASSIGN => normal assignment
 */

/**/
mod_export void
filesub(char **namptr, int assign)
{
    char *eql = NULL, *sub = NULL, *str, *ptr;
    int len;

    filesubstr(namptr, assign);

    if (!assign)
	return;

    if (assign & PF_TYPESET) {
	if ((*namptr)[1] && (eql = sub = strchr(*namptr + 1, Equals))) {
	    str = sub + 1;
	    if ((sub[1] == Tilde || sub[1] == Equals) && filesubstr(&str, assign)) {
		sub[1] = '\0';
		*namptr = dyncat(*namptr, str);
	    }
	} else
	    return;
    }

    ptr = *namptr;
    while ((sub = strchr(ptr, ':'))) {
	str = sub + 1;
	len = sub - *namptr;
	if (sub > eql &&
	    (sub[1] == Tilde || sub[1] == Equals) &&
	    filesubstr(&str, assign)) {
	    sub[1] = '\0';
	    *namptr = dyncat(*namptr, str);
	}
	ptr = *namptr + len + 1;
    }
}

#define isend(c) ( !(c) || (c)=='/' || (c)==Inpar || (assign && (c)==':') )
#define isend2(c) ( !(c) || (c)==Inpar || (assign && (c)==':') )

/*
 * do =foo substitution, or equivalent.
 * on entry, str should point to the "foo".
 * if assign, this is in an assignment
 * if nomatch, report hard error on failure.
 * if successful, returns the expansion, else NULL.
 */

/**/
char *
equalsubstr(char *str, int assign, int nomatch)
{
    char *pp, *cnam, *cmdstr, *ret;

    for (pp = str; !isend2(*pp); pp++)
	;
    cmdstr = dupstrpfx(str, pp-str);
    untokenize(cmdstr);
    remnulargs(cmdstr);
    if (!(cnam = findcmd(cmdstr, 1))) {
	if (nomatch)
	    zerr("%s not found", cmdstr);
	return NULL;
    }
    ret = dupstring(cnam);
    if (*pp)
	ret = dyncat(ret, pp);
    return ret;
}

/**/
mod_export int
filesubstr(char **namptr, int assign)
{
    char *str = *namptr;

    if (*str == Tilde && str[1] != '=' && str[1] != Equals) {
	Shfunc dirfunc;
	char *ptr, *tmp, *res, *ptr2;
	int val;

	val = zstrtol(str + 1, &ptr, 10);
	if (isend(str[1])) {   /* ~ */
	    *namptr = dyncat(home ? home : "", str + 1);
	    return 1;
	} else if (str[1] == '+' && isend(str[2])) {   /* ~+ */
	    *namptr = dyncat(pwd, str + 2);
	    return 1;
	} else if (str[1] == '-' && isend(str[2])) {   /* ~- */
	    *namptr = dyncat((tmp = oldpwd) ? tmp : pwd, str + 2);
	    return 1;
	} else if (str[1] == Inbrack &&
		   (dirfunc = getshfunc("zsh_directory_name")) &&
		   (ptr2 = strchr(str+2, Outbrack))) {
	    char **arr;
	    untokenize(tmp = dupstrpfx(str+2, ptr2 - (str+2)));
	    remnulargs(tmp);
	    arr = subst_string_by_func(dirfunc, "n", tmp);
	    res = arr ? *arr : NULL;
	    if (res) {
		*namptr = dyncat(res, ptr2+1);
		return 1;
	    }
	    if (isset(NOMATCH))
		zerr("no directory expansion: ~[%s]", tmp);
	    return 0;
	} else if (!inblank(str[1]) && isend(*ptr) &&
		   (!idigit(str[1]) || (ptr - str < 4))) {
	    char *ds;

	    if (val < 0)
		val = -val;
	    ds = dstackent(str[1], val);
	    if (!ds)
		return 0;
	    *namptr = dyncat(ds, ptr);
	    return 1;
	} else if ((ptr = itype_end(str+1, IUSER, 0)) != str+1) {   /* ~foo */
	    char *hom, save;

	    save = *ptr;
	    if (!isend(save))
		return 0;
	    *ptr = 0;
	    if (!(hom = getnameddir(++str))) {
		if (isset(NOMATCH))
		    zerr("no such user or named directory: %s", str);
		*ptr = save;
		return 0;
	    }
	    *ptr = save;
	    *namptr = dyncat(hom, ptr);
	    return 1;
	}
    } else if (*str == Equals && isset(EQUALS) && str[1]) {   /* =foo */
	char *expn = equalsubstr(str+1, assign, isset(NOMATCH));
	if (expn) {
	    *namptr = expn;
	    return 1;
	}
    }
    return 0;
}

#undef isend
#undef isend2

/**/
static char *
strcatsub(char **d, char *pb, char *pe, char *src, int l, char *s, int glbsub,
	  int copied)
{
    char *dest;
    int pl = pe - pb;

    if (!pl && (!s || !*s)) {
	*d = dest = (copied ? src : dupstring(src));
	if (glbsub)
	    shtokenize(dest);
    } else {
	*d = dest = hcalloc(pl + l + (s ? strlen(s) : 0) + 1);
	strncpy(dest, pb, pl);
	dest += pl;
	strcpy(dest, src);
	if (glbsub)
	    shtokenize(dest);
	dest += l;
	if (s)
	    strcpy(dest, s);
    }
    return dest;
}

/*
 * Pad the string str, returning a result from the heap (or str itself,
 * if it didn't need padding).  If str is too large, it will be truncated.
 * Calculations are in terms of width if MULTIBYTE is in effect and
 * multi_width is non-zero, else characters.
 *
 * prenum and postnum are the width to which the string needs padding
 * on the left and right.
 *
 * preone and postone are string to insert once only before and after
 * str.  They will be truncated on the left or right, respectively,
 * if necessary to fit the width.  Either or both may be NULL in which
 * case they will not be used.
 *
 * premul and postmul are the padding strings to be repeated before
 * on the left (if prenum is non-zero) and right (if postnum is non-zero).  If
 * NULL the first character of IFS (typically but not necessarily a space)
 * will be used.
 */

static char *
dopadding(char *str, int prenum, int postnum, char *preone, char *postone,
	  char *premul, char *postmul
#ifdef MULTIBYTE_SUPPORT
	  , int multi_width
#endif
    )
{
#ifdef MULTIBYTE_SUPPORT
#define WCPADWIDTH(cchar)	(multi_width ? WCWIDTH(cchar) : 1)
#else
#define WCPADWIDTH(cchar)	(1)
#endif

    char *def, *ret, *t, *r;
    int ls, ls2, lpreone, lpostone, lpremul, lpostmul, lr, f, m, c, cc, cl;
    convchar_t cchar;

    MB_METACHARINIT();
    if (!ifs || *ifs) {
	char *tmpifs = ifs ? ifs : DEFAULT_IFS;
	def = dupstrpfx(tmpifs, MB_METACHARLEN(tmpifs));
    } else
	def = "";
    if (preone && !*preone)
	preone = def;
    if (postone && !*postone)
	postone = def;
    if (!premul || !*premul)
	premul = def;
    if (!postmul || !*postmul)
	postmul = def;

    ls = MB_METASTRLEN2(str, multi_width);
    lpreone = preone ? MB_METASTRLEN2(preone, multi_width) : 0;
    lpostone = postone ? MB_METASTRLEN2(postone, multi_width) : 0;
    lpremul = MB_METASTRLEN2(premul, multi_width);
    lpostmul = MB_METASTRLEN2(postmul, multi_width);

    if (prenum + postnum == ls)
	return str;

    /*
     * Try to be careful with allocated lengths.  The following
     * is a maximum, in case we need the entire repeated string
     * for each repetition.  We probably don't, but in case the user
     * has given us something pathological which doesn't convert
     * easily into a width we'd better be safe.
     */
    lr = strlen(str) + strlen(premul) * prenum + strlen(postmul) * postnum;
    /*
     * Same logic for preone and postone, except those may be NULL.
     */
    if (preone)
	lr += strlen(preone);
    if (postone)
	lr += strlen(postone);
    r = ret = (char *)zhalloc(lr + 1);

    if (prenum) {
	/*
	 * Pad on the left.
	 */
	if (postnum) {
	    /*
	     * Pad on both right and left.
	     * The strategy is to divide the string into two halves.
	     * The first half is dealt with by the left hand padding
	     * code, the second by the right hand.
	     */
	    ls2 = ls / 2;

	    /* The width left to pad for the first half. */
	    f = prenum - ls2;
	    if (f <= 0) {
		/* First half doesn't fit.  Skip the first -f width. */
		f = -f;
		MB_METACHARINIT();
		while (f > 0) {
		    str += MB_METACHARLENCONV(str, &cchar);
		    f -= WCPADWIDTH(cchar);
		}
		/* Now finish the first half. */
		for (c = prenum; c > 0; ) {
		    cl = MB_METACHARLENCONV(str, &cchar);
		    while (cl--)
			*r++ = *str++;
		    c -= WCPADWIDTH(cchar);
		}
	    } else {
		if (f <= lpreone) {
		    if (preone) {
			/*
			 * The unrepeated string doesn't fit.
			 */
			MB_METACHARINIT();
			/* The width we need to skip */
			f = lpreone - f;
			/* So skip. */
			for (t = preone; f > 0; ) {
			    t += MB_METACHARLENCONV(t, &cchar);
			    f -= WCPADWIDTH(cchar);
			}
			/* Then copy the entire remainder. */
			while (*t)
			    *r++ = *t++;
		    }
		} else {
		    f -= lpreone;
		    if (lpremul) {
			if ((m = f % lpremul)) {
			    /*
			     * Left over fraction of repeated string.
			     */
			    MB_METACHARINIT();
			    /* Skip this much. */
			    m = lpremul - m;
			    for (t = premul; m > 0; ) {
				t += MB_METACHARLENCONV(t, &cchar);
				m -= WCPADWIDTH(cchar);
			    }
			    /* Output the rest. */
			    while (*t)
				*r++ = *t++;
			}
			for (cc = f / lpremul; cc--;) {
			    /* Repeat the repeated string */
			    MB_METACHARINIT();
			    for (c = lpremul, t = premul; c > 0; ) {
				cl = MB_METACHARLENCONV(t, &cchar);
				while (cl--)
				    *r++ = *t++;
				c -= WCPADWIDTH(cchar);
			    }
			}
		    }
		    if (preone) {
			/* Output the full unrepeated string */
			while (*preone)
			    *r++ = *preone++;
		    }
		}
		/* Output the first half width of the original string. */
		for (c = ls2; c > 0; ) {
		    cl = MB_METACHARLENCONV(str, &cchar);
		    c -= WCPADWIDTH(cchar);
		    while (cl--)
			*r++ = *str++;
		}
	    }
	    /* Other half.  In case the string had an odd length... */
	    ls2 = ls - ls2;
	    /* Width that needs padding... */
	    f = postnum - ls2;
	    if (f <= 0) {
		/* ...is negative, truncate original string */
		MB_METACHARINIT();
		for (c = postnum; c > 0; ) {
		    cl = MB_METACHARLENCONV(str, &cchar);
		    c -= WCPADWIDTH(cchar);
		    while (cl--)
			*r++ = *str++;
		}
	    } else {
		/* Rest of original string fits, output it complete */
		while (*str)
		    *r++ = *str++;
		if (f <= lpostone) {
		    if (postone) {
			/* Can't fit unrepeated string, truncate it */
			for (c = f; c > 0; ) {
			    cl = MB_METACHARLENCONV(postone, &cchar);
			    c -= WCPADWIDTH(cchar);
			    while (cl--)
				*r++ = *postone++;
			}
		    }
		} else {
		    if (postone) {
			f -= lpostone;
			/* Output entire unrepeated string */
			while (*postone)
			    *r++ = *postone++;
		    }
		    if (lpostmul) {
			for (cc = f / lpostmul; cc--;) {
			    /* Begin the beguine */
			    for (t = postmul; *t; )
				*r++ = *t++;
			}
			if ((m = f % lpostmul)) {
			    /* Fill leftovers with chunk of repeated string */
			    MB_METACHARINIT();
			    while (m > 0) {
				cl = MB_METACHARLENCONV(postmul, &cchar);
				m -= WCPADWIDTH(cchar);
				while (cl--)
				    *r++ = *postmul++;
			    }
			}
		    }
		}
	    }
	} else {
	    /*
	     * Pad only on the left.
	     */
	    f = prenum - ls;
	    if (f <= 0) {
		/*
		 * Original string is at least as wide as padding.
		 * Truncate original string to width.
		 * Truncate on left, so skip the characters we
		 * don't need.
		 */
		f = -f;
		MB_METACHARINIT();
		while (f > 0) {
		    str += MB_METACHARLENCONV(str, &cchar);
		    f -= WCPADWIDTH(cchar);
		}
		/* Copy the rest of the original string */
		for (c = prenum; c > 0; ) {
		    cl = MB_METACHARLENCONV(str, &cchar);
		    while (cl--)
			*r++ = *str++;
		    c -= WCPADWIDTH(cchar);
		}
	    } else {
		/*
		 * We can fit the entire string...
		 */
		if (f <= lpreone) {
		    if (preone) {
			/*
			 * ...with some fraction of the unrepeated string.
			 */
			/* We need this width of characters. */
			c = f;
			/*
			 * We therefore need to skip this width of
			 * characters.
			 */
			f = lpreone - f;
			MB_METACHARINIT();
			for (t = preone; f > 0; ) {
			    t += MB_METACHARLENCONV(t, &cchar);
			    f -= WCPADWIDTH(cchar);
			}
			/* Copy the rest of preone */
			while (*t)
			    *r++ = *t++;
		    }
		} else {
		    /*
		     * We can fit the whole of preone, needing this width
		     * first
		     */
		    f -= lpreone;
		    if (lpremul) {
			if ((m = f % lpremul)) {
			    /*
			     * Some fraction of the repeated string needed.
			     */
			    /* Need this much... */
			    c = m;
			    /* ...skipping this much first. */
			    m = lpremul - m;
			    MB_METACHARINIT();
			    for (t = premul; m > 0; ) {
				t += MB_METACHARLENCONV(t, &cchar);
				m -= WCPADWIDTH(cchar);
			    }
			    /* Now the rest of the repeated string. */
			    while (c > 0) {
				cl = MB_METACHARLENCONV(t, &cchar);
				while (cl--)
				    *r++ = *t++;
				c -= WCPADWIDTH(cchar);
			    }
			}
			for (cc = f / lpremul; cc--;) {
			    /*
			     * Repeat the repeated string.
			     */
			    MB_METACHARINIT();
			    for (c = lpremul, t = premul; c > 0; ) {
				cl = MB_METACHARLENCONV(t, &cchar);
				while (cl--)
				    *r++ = *t++;
				c -= WCPADWIDTH(cchar);
			    }
			}
		    }
		    if (preone) {
			/*
			 * Now the entire unrepeated string.  Don't
			 * count the width, just dump it.  This is
			 * significant if there are special characters
			 * in this string.  It's sort of a historical
			 * accident that this worked, but there's nothing
			 * to stop us just dumping the thing out and assuming
			 * the user knows what they're doing.
			 */
			while (*preone)
			    *r++ = *preone++;
		    }
		}
		/* Now the string being padded */
		while (*str)
		    *r++ = *str++;
	    }
	}
    } else if (postnum) {
	/*
	 * Pad on the right.
	 */
	f = postnum - ls;
	MB_METACHARINIT();
	if (f <= 0) {
	    /*
	     * Original string is at least as wide as padding.
	     * Truncate original string to width.
	     */
	    for (c = postnum; c > 0; ) {
		cl = MB_METACHARLENCONV(str, &cchar);
		while (cl--)
		    *r++ = *str++;
		c -= WCPADWIDTH(cchar);
	    }
	} else {
	    /*
	     * There's some space to fill.  First copy the original
	     * string, counting the width.  Make sure we copy the
	     * entire string.
	     */
	    for (c = ls; *str; ) {
		cl = MB_METACHARLENCONV(str, &cchar);
		while (cl--)
		    *r++ = *str++;
		c -= WCPADWIDTH(cchar);
	    }
	    MB_METACHARINIT();
	    if (f <= lpostone) {
		if (postone) {
		    /*
		     * Not enough or only just enough space to fit
		     * the unrepeated string.  Truncate as necessary.
		     */
		    for (c = f; c > 0; ) {
			cl = MB_METACHARLENCONV(postone, &cchar);
			while (cl--)
			    *r++ = *postone++;
			c -= WCPADWIDTH(cchar);
		    }
		}
	    } else {
		if (postone) {
		    f -= lpostone;
		    /* Copy the entire unrepeated string */
		    for (c = lpostone; *postone; ) {
			cl = MB_METACHARLENCONV(postone, &cchar);
			while (cl--)
			    *r++ = *postone++;
			c -= WCPADWIDTH(cchar);
		    }
		}
		if (lpostmul) {
		    /* Repeat the repeated string */
		    for (cc = f / lpostmul; cc--;) {
			MB_METACHARINIT();
			for (c = lpostmul, t = postmul; *t; ) {
			    cl = MB_METACHARLENCONV(t, &cchar);
			    while (cl--)
				*r++ = *t++;
			    c -= WCPADWIDTH(cchar);
			}
		    }
		    /*
		     * See if there's any fraction of the repeated
		     * string needed to fill up the remaining space.
		     */
		    if ((m = f % lpostmul)) {
			MB_METACHARINIT();
			while (m > 0) {
			    cl = MB_METACHARLENCONV(postmul, &cchar);
			    while (cl--)
				*r++ = *postmul++;
			    m -= WCPADWIDTH(cchar);
			}
		    }
		}
	    }
	}
    }
    *r = '\0';

    return ret;
}


/*
 * Look for a delimited portion of a string.  The first (possibly
 * multibyte) character at s is the delimiter.  Various forms
 * of brackets are treated separately, as documented.
 *
 * Returns a pointer to the final delimiter.  Sets *len to the
 * length of the final delimiter; a NULL causes *len to be set
 * to zero since we shouldn't advance past it.  (The string is
 * tokenized, so a NULL is a real end of string.)
 */

/**/
char *
get_strarg(char *s, int *lenp)
{
    convchar_t del;
    int len;
    char tok = 0;

    MB_METACHARINIT();
    len = MB_METACHARLENCONV(s, &del);
    if (!len) {
	*lenp = 0;
	return s;
    }

#ifdef MULTIBYTE_SUPPORT
    if (del == WEOF)
	del = (wint_t)((*s == Meta) ? s[1] ^ 32 : *s);
#endif
    s += len;
    switch (del) {
    case ZWC('('):
	del = ZWC(')');
	break;
    case '[':
	del = ZWC(']');
	break;
    case '{':
	del = ZWC('}');
	break;
    case '<':
	del = ZWC('>');
	break;
    case Inpar:
	tok = Outpar;
	break;
    case Inang:
	tok = Outang;
	break;
    case Inbrace:
	tok = Outbrace;
	break;
    case Inbrack:
	tok = Outbrack;
	break;
    }

    if (tok) {
	/*
	 * Looking for a matching token; we want the literal byte,
	 * not a decoded multibyte character, so search specially.
	 */
	while (*s && *s != tok)
	    s++;
    } else {
	convchar_t del2;
	len = 0;
	while (*s) {
	    len = MB_METACHARLENCONV(s, &del2);
#ifdef MULTIBYTE_SUPPORT
	    if (del2 == WEOF)
		del2 = (wint_t)((*s == Meta) ? s[1] ^ 32 : *s);
#endif
	    if (del == del2)
		break;
	    s += len;
	}
    }

    *lenp = len;
    return s;
}

/*
 * Get an integer argument; update *s to the end of the
 * final delimiter.  *delmatchp is set to the length of the
 * matched delimiter if we have matching, delimiters and there was no error in
 * the evaluation, else 0.
 */

/**/
static int
get_intarg(char **s, int *delmatchp)
{
    int arglen;
    char *t = get_strarg(*s, &arglen);
    char *p, sav;
    zlong ret;

    *delmatchp = 0;
    if (!*t)
	return -1;
    sav = *t;
    *t = '\0';
    p = dupstring(*s + arglen);
    *s = t + arglen;
    *t = sav;
    if (parsestr(p))
	return -1;
    singsub(&p);
    if (errflag)
	return -1;
    ret = mathevali(p);
    if (errflag)
	return -1;
    if (ret < 0)
	ret = -ret;
    *delmatchp = arglen;
    return ret < 0 ? -ret : ret;
}

/* Parsing for the (e) flag. */

static int
subst_parse_str(char **sp, int single, int err)
{
    char *s;

    *sp = s = dupstring(*sp);

    if (!(err ? parsestr(s) : parsestrnoerr(s))) {
	if (!single) {
            int qt = 0;

	    for (; *s; s++)
		if (!qt) {
		    if (*s == Qstring)
			*s = String;
		    else if (*s == Qtick)
			*s = Tick;
                } else if (*s == Dnull)
                    qt = !qt;
	}
	return 0;
    }
    return 1;
}

/* Evaluation for (#) flag */

static char *
substevalchar(char *ptr)
{
    zlong ires = mathevali(ptr);
    int len = 0;

    if (errflag)
	return NULL;
#ifdef MULTIBYTE_SUPPORT
    if (isset(MULTIBYTE) && ires > 127) {
	/* '\\' + 'U' + 8 bytes of character + '\0' */
	char buf[11];

	/* inefficient: should separate out \U handling from getkeystring */
	sprintf(buf, "\\U%.8x", (unsigned int)ires & 0xFFFFFFFFu);
	ptr = getkeystring(buf, &len, GETKEYS_BINDKEY, NULL);
    }
    if (len == 0)
#endif
    {
	ptr = zhalloc(2);
	len = 1;
	sprintf(ptr, "%c", (int)ires);
    }
    return metafy(ptr, len, META_USEHEAP);
}

/*
 * Helper function for arguments to parameter flags which
 * handles the (p) and (~) flags as escapes and tok_arg respectively.
 */

static char *
untok_and_escape(char *s, int escapes, int tok_arg)
{
    int klen;
    char *dst;

    untokenize(dst = dupstring(s));
    if (escapes) {
	dst = getkeystring(dst, &klen, GETKEYS_SEP, NULL);
	dst = metafy(dst, klen, META_HREALLOC);
    }
    if (tok_arg)
	shtokenize(dst);
    return dst;
}

/* parameter substitution */

#define	isstring(c) ((c) == '$' || (char)(c) == String || (char)(c) == Qstring)
#define isbrack(c)  ((c) == '[' || (char)(c) == Inbrack)

/*
 * Given a linked list l with node n, perform parameter substitution
 * starting from *str.  Return the node with the substitutuion performed
 * or NULL if it failed.
 *
 * If qt is true, the `$' was quoted.  TODO: why can't we just look
 * to see if the first character was String or Qstring?
 *
 * If ssub is true, we are being called via singsubst(), which means
 * the result will be a single word.  TODO: can we generate the
 * single word at the end?  TODO: if not, or maybe in any case,
 * can we pass down the ssub flag from prefork with the other flags
 * instead of pushing it into different arguments?  (How exactly
 * to qt and ssub differ?  Are both necessary, if so is there some
 * better way of separating the two?)
 */

/**/
static LinkNode
paramsubst(LinkList l, LinkNode n, char **str, int qt, int ssub)
{
    char *aptr = *str, c, cc;
    char *s = aptr, *fstr, *idbeg, *idend, *ostr = (char *) getdata(n);
    int colf;			/* != 0 means we found a colon after the name */
    /*
     * There are far too many flags.  They need to be grouped
     * together into some structure which ties them to where they
     * came from.
     *
     * Some flags have a an obscure relationship to their effect which
     * depends on incrementing them to particular values in particular
     * ways.
     */
    /*
     * Whether the value is an array (in aval) or not (in val).  There's
     * a movement from storing the value in the stuff read from the
     * parameter (the value v) to storing them in val and aval.
     * However, sometimes you find v reappearing temporarily.
     *
     * The values -1 and 2 are special to isarr.  The value -1 is used
     * to force us to keep an empty array.  It's tested in the YUK chunk
     * (I mean the one explicitly marked as such).  The value 2
     * indicates an array has come from splitting a scalar.  We use
     * that to override the usual rule that in double quotes we don't
     * remove empty elements (so "${(s.:):-foo::bar}" produces two
     * words).  This seems to me to be quite the wrong thing to do,
     * but it looks like code may be relying on it.  So we require (@)
     * as well before we keep the empty fields (look for assignments
     * like "isarr = nojoin ? 1 : 2").
     */
    int isarr = 0;
    /*
     * This is just the setting of the option except we need to
     * take account of ^ and ^^.
     */
    int plan9 = isset(RCEXPANDPARAM);
    /*
     * Likwise, but with ~ and ~~.  Also, we turn it off later
     * on if qt is passed down.
     */
    int globsubst = isset(GLOBSUBST);
    /*
     * Indicates ${(#)...}.
     */
    int evalchar = 0;
    /*
     * Indicates ${#pm}, massaged by whichlen which is set by
     * the (c), (w), and (W) flags to indicate how we take the length.
     */
    int getlen = 0;
    int whichlen = 0;
    /*
     * Indicates ${+pm}: a simple boolean for once.
     */
    int chkset = 0;
    /*
     * Indicates we have tried to get a value in v but that was
     * unset.  I don't quite understand why (v == NULL) isn't
     * good enough, but there are places where we seem to need
     * to second guess whether a value is a real value or not.
     */
    int vunset = 0;
    /*
     * Indicates (t) flag, i.e. print out types.  The code for
     * this actually isn't too horrifically inbred compared with
     * that for (P).
     */
    int wantt = 0;
    /*
     * Indicates spliting a string into an array.  There aren't
     * actually that many special cases for this --- which may
     * be why it doesn't work properly; we split in some cases
     * where we shouldn't, in particular on the multsubs for
     * handling embedded values for ${...=...} and the like.
     */
    int spbreak = isset(SHWORDSPLIT) && !ssub && !qt;
    /* Scalar and array value, see isarr above */
    char *val = NULL, **aval = NULL;
    /*
     * vbuf and v are both used to retrieve parameter values; this
     * is a kludge, we pass down vbuf and it may or may not return v.
     */
    struct value vbuf;
    Value v = NULL;
    /*
     * This expressive name refers to the set of flags which
     * is applied to matching for #, %, / and their doubled variants:
     * (M), (R), (B), (E), (N), (S).
     */
    int flags = 0;
    /* Value from (I) flag, used for ditto. */
    int flnum = 0;
    /*
     * sortit is to be passed to strmetasort().
     * indord is the (a) flag, which for consistency doesn't get
     * combined into sortit.
     */
    int sortit = SORTIT_ANYOLDHOW, indord = 0;
    /* (u): straightforward. */
    int unique = 0;
    /* combination of (L), (U) and (C) flags. */
    int casmod = CASMOD_NONE;
    /*
     * quotemod says we are doing either (q) (positive), (Q) (negative)
     * or not (0).  quotetype counts the q's for the first case.
     * quoterr is simply (X) but gets passed around a lot because the
     * combination (eX) needs it.
     */
    int quotemod = 0, quotetype = QT_NONE, quoteerr = 0;
    /*
     * (V) flag: fairly straightforward, except that as with so
     * many flags it's not easy to decide where to put it in the order.
     */
    int visiblemod = 0;
    /*
     * The (z) flag, nothing to do with SH_WORD_SPLIT which is tied
     * spbreak, see above; fairly straighforward in use but c.f.
     * the comment for visiblemod.
     */
    int shsplit = 0;
    /*
     * The separator from (j) and (s) respectively, or (F) and (f)
     * respectively (hardwired to "\n" in that case).  Slightly
     * confusingly also used for ${#pm}, thought that's at least
     * documented in the manual
     */
    char *sep = NULL, *spsep = NULL;
    /*
     * Padding strings.  The left and right padding strings which
     * are repeated, then the ones which only occur once, for
     * the (l) and (r) flags.
     */
    char *premul = NULL, *postmul = NULL, *preone = NULL, *postone = NULL;
    /* Replacement string for /orig/repl and //orig/repl */
    char *replstr = NULL;
    /* The numbers for (l) and (r) */
    zlong prenum = 0, postnum = 0;
#ifdef MULTIBYTE_SUPPORT
    /* The (m) flag: use width of multibyte characters */
    int multi_width = 0;
#endif
    /*
     * Whether the value has been copied.  Optimisation:  if we
     * are modifying an expression, we only need to copy it the
     * first time, and if we don't modify it we can just use the
     * value from the parameter or input.
     */
    int copied = 0;
    /*
     * The (A) flag for array assignment, with consequences for
     * splitting and joining; (AA) gives arrasg == 2 for associative
     * arrays.
     */
    int arrasg = 0;
    /*
     * The (e) flag.  As we need to do extra work not quite
     * at the end, the effect of this is kludged in in several places.
     */
    int eval = 0;
    /*
     * The (P) flag.  This interacts a bit obscurely with whether
     * or not we are dealing with a sub expression (subexp).
     */
    int aspar = 0;
    /*
     * The (%) flag, c.f. visiblemod again.
     */	
    int presc = 0;
    /*
     * The (@) flag; interacts obscurely with qt and isarr.
     * This is one of the things that decides whether multsub
     * will produce an array, but in an extremely indirect fashion.
     */
    int nojoin = 0;
    /*
     * != 0 means ${...}, otherwise $...  What works without braces
     * is largely a historical artefact (everything works with braces,
     * I sincerely hope).
     */
    char inbrace = 0;
    /*
     * Use for the (k) flag.  Goes down into the parameter code,
     * sometimes.
     */
    char hkeys = 0;
    /*
     * Used for the (v) flag, ditto.  Not quite sure why they're
     * separate, but the tradition seems to be that things only
     * get combined when that makes the result more obscure rather
     * than less.
     */
    char hvals = 0;
    /*
     * Whether we had to evaluate a subexpression, i.e. an
     * internal ${...} or $(...) or plain $pm.  We almost don't
     * need to remember this (which would be neater), but the (P)
     * flag means the subexp and !subexp code is obscurely combined,
     * and the argument passing to fetchvalue has another kludge.
     */
    int subexp;

    *s++ = '\0';
    /*
     * Nothing to do unless the character following the $ is
     * something we recognise.
     *
     * Shouldn't this be a table or something?  We test for all
     * these later on, too.
     */
    c = *s;
    if (itype_end(s, IIDENT, 1) == s && *s != '#' && c != Pound &&
	c != '-' && c != '!' && c != '$' && c != String && c != Qstring &&
	c != '?' && c != Quest &&
	c != '*' && c != Star && c != '@' && c != '{' &&
	c != Inbrace && c != '=' && c != Equals && c != Hat &&
	c != '^' && c != '~' && c != Tilde && c != '+') {
	s[-1] = '$';
	*str = s;
	return n;
    }
    DPUTS(c == '{', "BUG: inbrace == '{' in paramsubst()");
    /*
     * Extra processing if there is an opening brace: mostly
     * flags in parentheses, but also one ksh hack.
     */
    if (c == Inbrace) {
	inbrace = 1;
	s++;
	/*
	 * In ksh emulation a leading `!' is a special flag working
	 * sort of like our (k).
	 * TODO: this is one of very few cases tied directly to
	 * the emulation mode rather than an option.  Since ksh
	 * doesn't have parameter flags it might be neater to
	 * handle this with the ^, =, ~ stuff, below.
	 */
	if ((c = *s) == '!' && s[1] != Outbrace && EMULATION(EMULATE_KSH)) {
	    hkeys = SCANPM_WANTKEYS;
	    s++;
	} else if (c == '(' || c == Inpar) {
	    char *t, sav;
	    int tt = 0;
	    zlong num;
	    /*
	     * The (p) flag is only remembered within
	     * this block.  It says we do print-style handling
	     * on the values for flags, but only on those.
	     */
	    int escapes = 0;
	    /*
	     * '~' in parentheses caused tokenization of string arg:
	     * similar to (p).
	     */
	    int tok_arg = 0;

	    for (s++; (c = *s) != ')' && c != Outpar; s++, tt = 0) {
		int arglen;	/* length of modifier argument */
		int dellen;	/* length of matched delimiter, 0 if not */
		char *del0;	/* pointer to initial delimiter */

		switch (c) {
		case ')':
		case Outpar:
		    break;
		case '~':
		case Tilde:
		    tok_arg = !tok_arg;
		    break;
		case 'A':
		    ++arrasg;
		    break;
		case '@':
		    nojoin = 1;
		    break;
		case 'M':
		    flags |= SUB_MATCH;
		    break;
		case 'R':
		    flags |= SUB_REST;
		    break;
		case 'B':
		    flags |= SUB_BIND;
		    break;
		case 'E':
		    flags |= SUB_EIND;
		    break;
		case 'N':
		    flags |= SUB_LEN;
		    break;
		case 'S':
		    flags |= SUB_SUBSTR;
		    break;
		case 'I':
		    s++;
		    flnum = get_intarg(&s, &dellen);
		    if (flnum < 0)
			goto flagerr;
		    s--;
		    break;

		case 'L':
		    casmod = CASMOD_LOWER;
		    break;
		case 'U':
		    casmod = CASMOD_UPPER;
		    break;
		case 'C':
		    casmod = CASMOD_CAPS;
		    break;

		case 'o':
		    if (!sortit)
			sortit |= SORTIT_SOMEHOW; /* sort, no modifiers */
		    break;
		case 'O':
		    sortit |= SORTIT_BACKWARDS;
		    break;
		case 'i':
		    sortit |= SORTIT_IGNORING_CASE;
		    break;
		case 'n':
		    sortit |= SORTIT_NUMERICALLY;
		    break;
		case 'a':
		    sortit |= SORTIT_SOMEHOW;
		    indord = 1;
		    break;

		case 'V':
		    visiblemod++;
		    break;

		case 'q':
		    quotemod++, quotetype++;
		    break;
		case 'Q':
		    quotemod--;
		    break;
		case 'X':
		    quoteerr = 1;
		    break;

		case 'e':
		    eval = 1;
		    break;
		case 'P':
		    aspar = 1;
		    break;

		case 'c':
		    whichlen = 1;
		    break;
		case 'w':
		    whichlen = 2;
		    break;
		case 'W':
		    whichlen = 3;
		    break;

		case 'f':
		    spsep = "\n";
		    break;
		case 'F':
		    sep = "\n";
		    break;

		case '0':
		    spsep = zhalloc(3);
		    spsep[0] = Meta;
		    spsep[1] = '\0' ^ 32;
		    spsep[2] = '\0';
		    break;

		case 's':
		    tt = 1;
		/* fall through */
		case 'j':
		    t = get_strarg(++s, &arglen);
		    if (*t) {
			sav = *t;
			*t = '\0';
			if (tt)
			    spsep = untok_and_escape(s + arglen,
						     escapes, tok_arg);
			else
			    sep = untok_and_escape(s + arglen,
						   escapes, tok_arg);
			*t = sav;
			s = t + arglen - 1;
		    } else
			goto flagerr;
		    break;

		case 'l':
		    tt = 1;
		/* fall through */
		case 'r':
		    s++;
		    /* delimiter position */
		    del0 = s;
		    num = get_intarg(&s, &dellen);
		    if (num < 0)
			goto flagerr;
		    if (tt)
			prenum = num;
		    else
			postnum = num;
		    /* must have same delimiter if more arguments */
		    if (!dellen || memcmp(del0, s, dellen)) {
			/* decrement since loop will increment */
			s--;
			break;
		    }
		    t = get_strarg(s, &arglen);
		    if (!*t)
			goto flagerr;
		    sav = *t;
		    *t = '\0';
		    if (tt)
			premul = untok_and_escape(s + arglen, escapes,
						  tok_arg);
		    else
			postmul = untok_and_escape(s + arglen, escapes,
						   tok_arg);
		    *t = sav;
		    sav = *s;
		    s = t + arglen;
		    /* again, continue only if another start delimiter */
		    if (memcmp(del0, s, dellen)) {
			/* decrement since loop will increment */
			s--;
			break;
		    }
		    t = get_strarg(s, &arglen);
		    if (!*t)
			goto flagerr;
		    sav = *t;
		    *t = '\0';
		    if (tt)
			preone = untok_and_escape(s + arglen,
						  escapes, tok_arg);
		    else
			postone = untok_and_escape(s + arglen,
						   escapes, tok_arg);
		    *t = sav;
		    /* -1 since loop will increment */
		    s = t + arglen - 1;
		    break;

		case 'm':
#ifdef MULTIBYTE_SUPPORT
		    multi_width = 1;
#endif
		    break;

		case 'p':
		    escapes = 1;
		    break;

		case 'k':
		    hkeys = SCANPM_WANTKEYS;
		    break;
		case 'v':
		    hvals = SCANPM_WANTVALS;
		    break;

		case 't':
		    wantt = 1;
		    break;

		case '%':
		    presc++;
		    break;

		case 'z':
		    shsplit = 1;
		    break;

		case 'u':
		    unique = 1;
		    break;

		case '#':
		case Pound:
		    evalchar = 1;
		    break;

		default:
		  flagerr:
		    zerr("error in flags");
		    return NULL;
		}
	    }
	    s++;
	}
    }

    /*
     * premul, postmul specify the padding character to be used
     * multiple times with the (l) and (r) flags respectively.
     */
    if (!premul)
	premul = " ";
    if (!postmul)
	postmul = " ";

    /*
     * Look for special unparenthesised flags.
     * TODO: could make these able to appear inside parentheses, too,
     * i.e. ${(^)...} etc.
     */
    for (;;) {
	if ((c = *s) == '^' || c == Hat) {
	    /* RC_EXPAND_PARAM on or off (doubled )*/
	    if ((c = *++s) == '^' || c == Hat) {
		plan9 = 0;
		s++;
	    } else
		plan9 = 1;
	} else if ((c = *s) == '=' || c == Equals) {
	    /* SH_WORD_SPLIT on or off (doubled). spbreak = 2 means force */
	    if ((c = *++s) == '=' || c == Equals) {
		spbreak = 0;
		s++;
	    } else
		spbreak = 2;
	} else if ((c == '#' || c == Pound) &&
		   (itype_end(s+1, IIDENT, 0) != s + 1
		    || (cc = s[1]) == '*' || cc == Star || cc == '@'
		    || cc == '-' || (cc == ':' && s[2] == '-')
		    || (isstring(cc) && (s[2] == Inbrace || s[2] == Inpar)))) {
	    getlen = 1 + whichlen, s++;
	    /*
	     * Return the length of the parameter.
	     * getlen can be more than 1 to indicate characters (2),
	     * words ignoring multiple delimiters (3), words taking
	     * account of multiple delimiters.  delimiter is in
	     * spsep, NULL means $IFS.
	     */
	} else if (c == '~' || c == Tilde) {
	    /* GLOB_SUBST on or off (doubled) */
	    if ((c = *++s) == '~' || c == Tilde) {
		globsubst = 0;
		s++;
	    } else
		globsubst = 1;
	} else if (c == '+') {
	    /*
	     * Return whether indicated parameter is set. 
	     * Try to handle this when parameter is named
	     * by (P) (second part of test).
	     */
	    if (itype_end(s+1, IIDENT, 0) != s+1 || (aspar && isstring(s[1]) &&
				 (s[2] == Inbrace || s[2] == Inpar)))
		chkset = 1, s++;
	    else if (!inbrace) {
		/* Special case for `$+' on its own --- leave unmodified */
		*aptr = '$';
		*str = aptr + 1;
		return n;
	    } else {
		zerr("bad substitution");
		return NULL;
	    }
	} else if (inbrace && inull(*s)) {
	    /*
	     * Handles things like ${(f)"$(<file)"} by skipping 
	     * the double quotes.  We don't need to know what was
	     * actually there; the presence of a String or Qstring
	     * is good enough.
	     */
	    s++;
	} else
	    break;
    }
    /* Don't activate special pattern characters if inside quotes */
    globsubst = globsubst && !qt;

    /*
     * At this point, we usually expect a parameter name.
     * However, there may be a nested ${...} or $(...).
     * These say that the parameter itself is somewhere inside,
     * or that there isn't a parameter and we will get the values
     * from a command substitution itself.  In either case,
     * the current instance of paramsubst() doesn't fetch a value,
     * it just operates on what gets passed up.
     * (The first ought to have been {...}, reserving ${...}
     * for substituting a value at that point, but it's too late now.)
     */
    idbeg = s;
    if ((subexp = (inbrace && s[-1] && isstring(*s) &&
		   (s[1] == Inbrace || s[1] == Inpar)))) {
	int sav;
	int quoted = *s == Qstring;

	val = s++;
	skipparens(*s, *s == Inpar ? Outpar : Outbrace, &s);
	sav = *s;
	*s = 0;
	/*
	 * This handles arrays.  TODO: this is not the most obscure call to
	 * multsub() (see below) but even so it would be nicer to pass down
	 * and back the arrayness more rationally.  In that case, we should
	 * remove the aspar test and extract a value from an array, if
	 * necessary, when we handle (P) lower down.
	 */
	if (multsub(&val, 0, (aspar ? NULL : &aval), &isarr, NULL) && quoted) {
	    /* Empty quoted string --- treat as null string, not elided */
	    isarr = -1;
	    aval = (char **) hcalloc(sizeof(char *));
	    aspar = 0;
	} else if (aspar)
	    idbeg = val;
	*s = sav;
	/*
	 * This tests for the second double quote in an expression
	 * like ${(f)"$(<file)"}, compare above.
	 */
	while (inull(*s))
	    s++;
	v = (Value) NULL;
    } else if (aspar) {
	/*
	 * No subexpression, but in any case the value is going
	 * to give us the name of a parameter on which we do
	 * our remaining processing.  In other words, this
	 * makes ${(P)param} work like ${(P)${param}}.  (Probably
	 * better looked at, this is the basic code for ${(P)param}
	 * and it's been kludged into the subexp code because no
	 * opportunity for a kludge has been neglected.)
	 */
	if ((v = fetchvalue(&vbuf, &s, 1, (qt ? SCANPM_DQUOTED : 0)))) {
	    val = idbeg = getstrvalue(v);
	    subexp = 1;
	} else
	    vunset = 1;
    }
    /*
     * We need to retrieve a value either if we haven't already
     * got it from a subexpression, or if the processing so
     * far has just yielded us a parameter name to be processed
     * with (P).
     */
    if (!subexp || aspar) {
	char *ov = val;

	/*
	 * Second argument: decide whether to use the subexpression or
	 *   the string next on the line as the parameter name.
	 * Third argument:  decide how processing for brackets
	 *   1 means full processing
	 *   -1 appears to mean something along the lines of
	 *     only handle single digits and don't handle brackets.
	 *     I *think* (but it's really only a guess) that this
	 *     is used by the test below the wantt handling, so
	 *     that in certain cases we handle brackets there.
	 *   0 would apparently mean something like we know we
	 *     should have the name of a scalar and we get cross
	 *     if there's anything present which disagrees with that
	 * but you will search fetchvalue() in vain for comments on this.
	 * Fourth argument gives flags to do with keys, values, quoting,
	 * assigning depending on context and parameter flags.
	 *
	 * This is the last mention of subexp, so presumably this
	 * is what the code which makes sure subexp is set if aspar (the
	 * (P) flag) is set.  I *think* what's going on here is the
	 * second argument is for both input and output: with
	 * subexp, we only want the input effect, whereas normally
	 * we let fetchvalue set the main string pointer s to
	 * the end of the bit it's fetched.
	 */
	if (!(v = fetchvalue(&vbuf, (subexp ? &ov : &s),
			     (wantt ? -1 :
			      ((unset(KSHARRAYS) || inbrace) ? 1 : -1)),
			     hkeys|hvals|
			     (arrasg ? SCANPM_ASSIGNING : 0)|
			     (qt ? SCANPM_DQUOTED : 0))) ||
	    (v->pm && (v->pm->node.flags & PM_UNSET)) ||
	    (v->flags & VALFLAG_EMPTY))
	    vunset = 1;

	if (wantt) {
	    /*
	     * Handle the (t) flag: value now becomes the type
	     * information for the parameter.
	     */
	    if (v && v->pm && !(v->pm->node.flags & PM_UNSET)) {
		int f = v->pm->node.flags;

		switch (PM_TYPE(f)) {
		case PM_SCALAR:  val = "scalar"; break;
		case PM_ARRAY:   val = "array"; break;
		case PM_INTEGER: val = "integer"; break;
		case PM_EFLOAT:
		case PM_FFLOAT:  val = "float"; break;
		case PM_HASHED:  val = "association"; break;
		}
		val = dupstring(val);
		if (v->pm->level)
		    val = dyncat(val, "-local");
		if (f & PM_LEFT)
		    val = dyncat(val, "-left");
		if (f & PM_RIGHT_B)
		    val = dyncat(val, "-right_blanks");
		if (f & PM_RIGHT_Z)
		    val = dyncat(val, "-right_zeros");
		if (f & PM_LOWER)
		    val = dyncat(val, "-lower");
		if (f & PM_UPPER)
		    val = dyncat(val, "-upper");
		if (f & PM_READONLY)
		    val = dyncat(val, "-readonly");
		if (f & PM_TAGGED)
		    val = dyncat(val, "-tag");
		if (f & PM_EXPORTED)
		    val = dyncat(val, "-export");
		if (f & PM_UNIQUE)
		    val = dyncat(val, "-unique");
		if (f & PM_HIDE)
		    val = dyncat(val, "-hide");
		if (f & PM_HIDE)
		    val = dyncat(val, "-hideval");
		if (f & PM_SPECIAL)
		    val = dyncat(val, "-special");
		vunset = 0;
	    } else
		val = dupstring("");

	    v = NULL;
	    isarr = 0;
	}
    }
    /*
     * We get in here two ways; either we need to convert v into
     * the local value system, or we need to get rid of brackets
     * even if there isn't a v.
     */
    while (v || ((inbrace || (unset(KSHARRAYS) && vunset)) && isbrack(*s))) {
	if (!v) {
	    /*
	     * Index applied to non-existent parameter; we may or may
	     * not have a value to index, however.  Create a temporary
	     * empty parameter as a trick, and index on that.  This
	     * usually happens the second time around the loop when
	     * we've used up the original parameter value and want to
	     * apply a subscript to what's left.  However, it's also
	     * possible it's got something to do with some of that murky
	     * passing of -1's as the third argument to fetchvalue() to
	     * inhibit bracket parsing at that stage.
	     */
	    Param pm;
	    char *os = s;

	    if (!isbrack(*s))
		break;
	    if (vunset) {
		val = dupstring("");
		isarr = 0;
	    }
	    pm = createparam(nulstring, isarr ? PM_ARRAY : PM_SCALAR);
	    DPUTS(!pm, "BUG: parameter not created");
	    if (isarr)
		pm->u.arr = aval;
	    else
		pm->u.str = val;
	    v = (Value) hcalloc(sizeof *v);
	    v->isarr = isarr;
	    v->pm = pm;
	    v->end = -1;
	    if (getindex(&s, v, qt ? SCANPM_DQUOTED : 0) || s == os)
		break;
	}
	/*
	 * This is where we extract a value (we know now we have
	 * one) into the local parameters for a scalar (val) or
	 * array (aval) value.  TODO: move val and aval into
	 * a structure with a discriminator.  Hope we can make
	 * more things array values at this point and dearrayify later.
	 * v->isarr tells us whether the stuff from down below looks
	 * like an array.
	 *
	 * I think we get to discard the existing value of isarr
	 * here because it's already been taken account of, either
	 * in the subexp stuff or immediately above.
	 */
	if ((isarr = v->isarr)) {
	    /*
	     * No way to get here with v->flags & VALFLAG_INV, so
	     * getvaluearr() is called by getarrvalue(); needn't test
	     * PM_HASHED.
	     */
	    if (v->isarr == SCANPM_WANTINDEX) {
		isarr = v->isarr = 0;
		val = dupstring(v->pm->node.nam);
	    } else
		aval = getarrvalue(v);
	} else {
	    /* Value retrieved from parameter/subexpression is scalar */
	    if (v->pm->node.flags & PM_ARRAY) {
		/*
		 * Although the value is a scalar, the parameter
		 * itself is an array.  Presumably this is due to
		 * being quoted, or doing single substitution or something,
		 * TODO: we're about to do some definitely stringy
		 * stuff, so something like this bit is probably
		 * necessary.  However, I'd like to leave any
		 * necessary joining of arrays until this point
		 * to avoid the multsub() horror.
		 */
		int tmplen = arrlen(v->pm->gsu.a->getfn(v->pm));

		if (v->start < 0)
		    v->start += tmplen + ((v->flags & VALFLAG_INV) ? 1 : 0);
		if (!(v->flags & VALFLAG_INV) &&
		    (v->start >= tmplen || v->start < 0))
		    vunset = 1;
	    }
	    if (!vunset) {
		/*
		 * There really is a value.  Padding and case
		 * transformations used to be handled here, but
		 * are now handled in getstrvalue() for greater
		 * consistency.  However, we get unexpected effects
		 * if we allow them to applied on every call, so
		 * set the flag that allows them to be substituted.
		 */
		v->flags |= VALFLAG_SUBST;
		val = getstrvalue(v);
	    }
	}
	/*
	 * Finished with the original parameter and its indices;
	 * carry on looping to see if we need to do more indexing.
	 * This means we final get rid of v in favour of val and
	 * aval.  We could do with somehow encapsulating the bit
	 * where we need v.
	 */
	v = NULL;
	if (!inbrace)
	    break;
    }
    /*
     * We're now past the name or subexpression; the only things
     * which can happen now are a closing brace, one of the standard
     * parameter postmodifiers, or a history-style colon-modifier.
     *
     * Again, this duplicates tests for characters we're about to
     * examine properly later on.
     */
    if (inbrace &&
	(c = *s) != '-' && c != '+' && c != ':' && c != '%'  && c != '/' &&
	c != '=' && c != Equals &&
	c != '#' && c != Pound &&
	c != '?' && c != Quest &&
	c != '}' && c != Outbrace) {
	zerr("bad substitution");
	return NULL;
    }
    /*
     * Join arrays up if we're in quotes and there isn't some
     * override such as (@).
     * TODO: hmm, if we're called as part of some recursive
     * substitution do we want to delay this until we get back to
     * the top level?  Or is if there's a qt (i.e. this parameter
     * substitution is in quotes) always good enough?  Potentially
     * we may be OK by now --- all potential `@'s and subexpressions
     * have been handled, including any [@] index which comes up
     * by virture of v->isarr being set to SCANPM_ISVAR_AT which
     * is now in isarr.
     *
     * However, if we are replacing multsub() with something that
     * doesn't mangle arrays, we may need to delay this step until after
     * the foo:- or foo:= or whatever that causes that.  Note the value
     * (string or array) at this point is irrelevant if we are going to
     * be doing that.  This would mean // and stuff get applied
     * arraywise even if quoted.  That's probably wrong, so maybe
     * this just stays.
     *
     * We do a separate stage of dearrayification in the YUK chunk,
     * I think mostly because of the way we make array or scalar
     * values appear to the caller.
     */
    if (isarr) {
	if (nojoin)
	    isarr = -1;
	if (qt && !getlen && isarr > 0) {
	    val = sepjoin(aval, sep, 1);
	    isarr = 0;
	}
    }

    idend = s;
    if (inbrace) {
	/*
	 * This is to match a closing double quote in case
	 * we didn't have a subexpression, e.g. ${"foo"}.
	 * This form is pointless, but logically it ought to work.
	 */
	while (inull(*s))
	    s++;
    }
    /*
     * We don't yet know whether a `:' introduces a history-style
     * colon modifier or qualifies something like ${...:=...}.
     * But if we remember the colon here it's easy to check later.
     */
    if ((colf = *s == ':'))
	s++;


    /* fstr is to be the text following the substitution.  If we have *
     * braces, we look for it here, else we infer it later on.        */
    fstr = s;
    if (inbrace) {
	int bct;
	for (bct = 1; (c = *fstr); fstr++) {
	    if (c == Inbrace)
		bct++;
	    else if (c == Outbrace && !--bct)
		break;
	}

	if (bct) {
	noclosebrace:
	    zerr("closing brace expected");
	    return NULL;
	}
	if (c)
	    *fstr++ = '\0';
    }

    /* Check for ${..?..} or ${..=..} or one of those. *
     * Only works if the name is in braces.            */

    if (inbrace && ((c = *s) == '-' ||
		    c == '+' ||
		    c == ':' ||	/* i.e. a doubled colon */
		    c == '=' || c == Equals ||
		    c == '%' ||
		    c == '#' || c == Pound ||
		    c == '?' || c == Quest ||
		    c == '/')) {

	/*
	 * Default index is 1 if no (I) or (I) gave zero.   But
	 * why don't we set the default explicitly at the start
	 * and massage any passed index where we set flnum anyway?
	 */
	if (!flnum)
	    flnum++;
	if (c == '%')
	    flags |= SUB_END;

	/* Check for ${..%%..} or ${..##..} */
	if ((c == '%' || c == '#' || c == Pound) && c == s[1]) {
	    s++;
	    /* we have %%, not %, or ##, not # */
	    flags |= SUB_LONG;
	}
	s++;
	if (s[-1] == '/') {
	    char *ptr;
	    /*
	     * previous flags are irrelevant, except for (S) which
	     * indicates shortest substring; else look for longest.
	     */
	    flags = (flags & SUB_SUBSTR) ? 0 : SUB_LONG;
	    if ((c = *s) == '/') {
		/* doubled, so replace all occurrences */
		flags |= SUB_GLOBAL;
		c = *++s;
	    }
	    /* Check for anchored substitution */
	    if (c == '#' || c == Pound) {
		/*
		 * anchor at head: this is the `normal' case in
		 * getmatch and we only require the flag if SUB_END
		 * is also present.
		 */
		flags |= SUB_START;
		s++;
	    }
	    if (*s == '%') {
		/* anchor at tail */
		flags |= SUB_END;
		s++;
	    }
	    if (!(flags & (SUB_START|SUB_END))) {
		/* No anchor, so substring */
		flags |= SUB_SUBSTR;
	    }
	    /*
	     * Find the / marking the end of the search pattern.
	     * If there isn't one, we're just going to delete that,
	     * i.e. replace it with an empty string.
	     *
	     * We used to use double backslashes to quote slashes,
	     * but actually that was buggy and using a single backslash
	     * is easier and more obvious.
	     */
	    for (ptr = s; (c = *ptr) && c != '/'; ptr++)
	    {
		if ((c == Bnull || c == Bnullkeep || c == '\\') && ptr[1])
		{
		    if (ptr[1] == '/')
			chuck(ptr);
		    else
			ptr++;
		}
	    }
	    replstr = (*ptr && ptr[1]) ? ptr+1 : "";
	    *ptr = '\0';
	}

	/* See if this was ${...:-...}, ${...:=...}, etc. */
	if (colf)
	    flags |= SUB_ALL;
	/*
	 * With no special flags, i.e. just a # or % or whatever,
	 * the matched portion is removed and we keep the rest.
	 * We also want the rest when we're doing a substitution.
	 */
	if (!(flags & (SUB_MATCH|SUB_REST|SUB_BIND|SUB_EIND|SUB_LEN)))
	    flags |= SUB_REST;

	if (colf && !vunset)
	    vunset = (isarr) ? !*aval : !*val || (*val == Nularg && !val[1]);

	switch (s[-1]) {
	case '+':
	    if (vunset) {
		val = dupstring("");
		copied = 1;
		isarr = 0;
		break;
	    }
	    vunset = 1;
	/* Fall Through! */
	case '-':
	    if (vunset) {
		int ws = opts[SHWORDSPLIT];
		val = dupstring(s);
		/* If word-splitting is enabled, we ask multsub() to split
		 * the substituted string at unquoted whitespace.  Then, we
		 * turn off spbreak so that no further splitting occurs.
		 * This allows a construct such as ${1+"$@"} to correctly
		 * keep its array splits, and weird constructs such as
		 * ${str+"one two" "3 2 1" foo "$str"} to only be split
		 * at the unquoted spaces. */
		opts[SHWORDSPLIT] = spbreak;
		multsub(&val, spbreak && !aspar, (aspar ? NULL : &aval), &isarr, NULL);
		opts[SHWORDSPLIT] = ws;
		copied = 1;
		spbreak = 0;
	    }
	    break;
	case ':':
	    /* this must be `::=', unconditional assignment */
	    if (*s != '=' && *s != Equals)
		goto noclosebrace;
	    vunset = 1;
	    s++;
	    /* Fall through */
	case '=':
	case Equals:
	    if (vunset) {
		int ws = opts[SHWORDSPLIT];
		char sav = *idend;
		int l;

		*idend = '\0';
		val = dupstring(s);
		if (spsep || !arrasg) {
		    opts[SHWORDSPLIT] = 0;
		    multsub(&val, 0, NULL, &isarr, NULL);
		} else {
		    opts[SHWORDSPLIT] = spbreak;
		    multsub(&val, spbreak, &aval, &isarr, NULL);
		    spbreak = 0;
		}
		opts[SHWORDSPLIT] = ws;
		if (arrasg) {
		    /* This is an array assignment. */
		    char *arr[2], **t, **a, **p;
		    if (spsep || spbreak) {
			aval = sepsplit(val, spsep, 0, 1);
			isarr = nojoin ? 1 : 2;
			l = arrlen(aval);
			if (l && !*(aval[l-1]))
			    l--;
			if (l && !**aval)
			    l--, t = aval + 1;
			else
			    t = aval;
		    } else if (!isarr) {
			if (!*val && arrasg > 1) {
			    arr[0] = NULL;
			    l = 0;
			} else {
			    arr[0] = val;
			    arr[1] = NULL;
			    l = 1;
			}
			t = aval = arr;
		    } else
			l = arrlen(aval), t = aval;
		    p = a = zalloc(sizeof(char *) * (l + 1));
		    while (l--) {
			untokenize(*t);
			*p++ = ztrdup(*t++);
		    }
		    *p++ = NULL;
		    if (arrasg > 1) {
			Param pm = sethparam(idbeg, a);
			if (pm)
			    aval = paramvalarr(pm->gsu.h->getfn(pm), hkeys|hvals);
		    } else
			setaparam(idbeg, a);
		} else {
		    untokenize(val);
		    setsparam(idbeg, ztrdup(val));
		}
		*idend = sav;
		copied = 1;
		if (isarr) {
		  if (nojoin)
		    isarr = -1;
		  if (qt && !getlen && isarr > 0 && !spsep && spbreak < 2) {
		    val = sepjoin(aval, sep, 1);
		    isarr = 0;
		  }
		  sep = spsep = NULL;
		  spbreak = 0;
		}
	    }
	    break;
	case '?':
	case Quest:
	    if (vunset) {
		*idend = '\0';
		zerr("%s: %s", idbeg, *s ? s : "parameter not set");
		if (!interact) {
		    if (mypid == getpid()) {
			/*
			 * paranoia: don't check for jobs, but there shouldn't
			 * be any if not interactive.
			 */
			stopmsg = 1;
			zexit(1, 0);
		    } else
			_exit(1);
		}
		return NULL;
	    }
	    break;
	case '%':
	case '#':
	case Pound:
	case '/':
            /* This once was executed only `if (qt) ...'. But with that
             * patterns in a expansion resulting from a ${(e)...} aren't
             * tokenized even though this function thinks they are (it thinks
             * they are because parse_subst_str() turns Qstring tokens
             * into String tokens and for unquoted parameter expansions the
             * lexer normally does tokenize patterns inside parameter
             * expansions). */
            {
		int one = noerrs, oef = errflag, haserr;

		if (!quoteerr)
		    noerrs = 1;
		haserr = parse_subst_string(s);
		noerrs = one;
		if (!quoteerr) {
		    errflag = oef;
		    if (haserr)
			shtokenize(s);
		} else if (haserr || errflag) {
		    zerr("parse error in ${...%c...} substitution", s[-1]);
		    return NULL;
		}
	    }
	    {
#if 0
		/*
		 * This allows # and % to be at the start of
		 * a parameter in the substitution, which is
		 * a bit nasty, and can be done (although
		 * less efficiently) with anchors.
		 */

		char t = s[-1];

		singsub(&s);

		if (t == '/' && (flags & SUB_SUBSTR)) {
		    if ((c = *s) == '#' || c == '%') {
			flags &= ~SUB_SUBSTR;
			if (c == '%')
			    flags |= SUB_END;
			s++;
		    } else if (c == '\\') {
			s++;
		    }
		}
#else
		singsub(&s);
#endif
	    }

	    /*
	     * Either loop over an array doing replacements or
	     * do the replacment on a string.
	     *
	     * We need an untokenized value for matching.
	     */
	    if (!vunset && isarr) {
		char **ap;
		if (!copied) {
		    aval = arrdup(aval);
		    copied = 1;
		}
		for (ap = aval; *ap; ap++) {
		    untokenize(*ap);
		}
		getmatcharr(&aval, s, flags, flnum, replstr);
	    } else {
		if (vunset)
		    val = dupstring("");
		if (!copied) {
		    val = dupstring(val);
		    copied = 1;
		    untokenize(val);
		}
		getmatch(&val, s, flags, flnum, replstr);
	    }
	    break;
	}
    } else {			/* no ${...=...} or anything, but possible modifiers. */
	/*
	 * Handler ${+...}.  TODO: strange, why do we handle this only
	 * if there isn't a trailing modifier?  Why don't we do this
	 * e.g. when we handle the ${(t)...} flag?
	 */
	if (chkset) {
	    val = dupstring(vunset ? "0" : "1");
	    isarr = 0;
	} else if (vunset) {
	    if (unset(UNSET)) {
		*idend = '\0';
		zerr("%s: parameter not set", idbeg);
		return NULL;
	    }
	    val = dupstring("");
	}
	if (colf) {
	    /*
	     * History style colon modifiers.  May need to apply
	     * on multiple elements of an array.
	     */
	    s--;
	    if (unset(KSHARRAYS) || inbrace) {
		if (!isarr)
		    modify(&val, &s);
		else {
		    char *ss;
		    char **ap = aval;
		    char **pp = aval = (char **) hcalloc(sizeof(char *) *
							 (arrlen(aval) + 1));

		    while ((*pp = *ap++)) {
			ss = s;
			modify(pp++, &ss);
		    }
		    if (pp == aval) {
			char *t = "";
			ss = s;
			modify(&t, &ss);
		    }
		    s = ss;
		}
		copied = 1;
		if (inbrace && *s) {
		    if (*s == ':' && !imeta(s[1]))
			zerr("unrecognized modifier `%c'", s[1]);
		    else
			zerr("unrecognized modifier");
		    return NULL;
		}
	    }
	}
	if (!inbrace)
	    fstr = s;
    }
    if (errflag)
	return NULL;
    if (evalchar) {
	int one = noerrs, oef = errflag, haserr = 0;

	if (!quoteerr)
	    noerrs = 1;
	/*
	 * Evaluate the value numerically and output the result as
	 * a character.
	 */
	if (isarr) {
	    char **aval2, **avptr, **av2ptr;

	    aval2 = (char **)zhalloc((arrlen(aval)+1)*sizeof(char *));

	    for (avptr = aval, av2ptr = aval2; *avptr; avptr++, av2ptr++)
	    {
		/* When noerrs = 1, the only error is out-of-memory */
		if (!(*av2ptr = substevalchar(*avptr))) {
		    haserr = 1;
		    break;
		}
	    }
	    *av2ptr = NULL;
	    aval = aval2;
	} else {
	    /* When noerrs = 1, the only error is out-of-memory */
	    if (!(val = substevalchar(val)))
		haserr = 1;
	}
	noerrs = one;
	if (!quoteerr)
	    errflag = oef;
	if (haserr || errflag)
	    return NULL;
    }
    /*
     * This handles taking a length with ${#foo} and variations.
     * TODO: again. one might naively have thought this had the
     * same sort of effect as the ${(t)...} flag and the ${+...}
     * test, although in this case we do need the value rather
     * the the parameter, so maybe it's a bit different.
     */
    if (getlen) {
	long len = 0;
	char buf[14];

	if (isarr) {
	    char **ctr;
	    int sl = sep ? MB_METASTRLEN(sep) : 1;

	    if (getlen == 1)
		for (ctr = aval; *ctr; ctr++, len++);
	    else if (getlen == 2) {
		if (*aval)
		    for (len = -sl, ctr = aval;
			 len += sl + MB_METASTRLEN2(*ctr, multi_width),
			     *++ctr;);
	    }
	    else
		for (ctr = aval;
		     *ctr;
		     len += wordcount(*ctr, spsep, getlen > 3), ctr++);
	} else {
	    if (getlen < 3)
		len = MB_METASTRLEN2(val, multi_width);
	    else
		len = wordcount(val, spsep, getlen > 3);
	}

	sprintf(buf, "%ld", len);
	val = dupstring(buf);
	isarr = 0;
    }
    /* At this point we make sure that our arrayness has affected the
     * arrayness of the linked list.  Then, we can turn our value into
     * a scalar for convenience sake without affecting the arrayness
     * of the resulting value. */
    if (isarr)
	l->list.flags |= LF_ARRAY;
    else
	l->list.flags &= ~LF_ARRAY;
    if (isarr > 0 && !plan9 && (!aval || !aval[0])) {
	val = dupstring("");
	isarr = 0;
    } else if (isarr && aval && aval[0] && !aval[1]) {
	/* treat a one-element array as a scalar for purposes of   *
	 * concatenation with surrounding text (some${param}thing) *
	 * and rc_expand_param handling.  Note: LF_ARRAY (above)   *
	 * propagates the true array type from nested expansions.  */
	val = aval[0];
	isarr = 0;
    }
    /* This is where we may join arrays together, e.g. (j:,:) sets "sep", and
     * (afterward) may split the joined value (e.g. (s:-:) sets "spsep").  One
     * exception is that ${name:-word} and ${name:+word} will have already
     * done any requested splitting of the word value with quoting preserved.
     * "ssub" is true when we are called from singsub (via prefork):
     * it means that we must join arrays and should not split words. */
    if (ssub || spbreak || spsep || sep) {
	if (isarr) {
	    val = sepjoin(aval, sep, 1);
	    isarr = 0;
	}
	if (!ssub && (spbreak || spsep)) {
	    aval = sepsplit(val, spsep, 0, 1);
	    if (!aval || !aval[0])
		val = dupstring("");
	    else if (!aval[1])
		val = aval[0];
	    else
		isarr = nojoin ? 1 : 2;
	}
	if (isarr)
	    l->list.flags |= LF_ARRAY;
	else
	    l->list.flags &= ~LF_ARRAY;
    }
    /*
     * Perform case modififications.
     */
    if (casmod != CASMOD_NONE) {
	copied = 1;		/* string is always modified by copy */
	if (isarr) {
	    char **ap, **ap2;

	    ap = aval;
	    ap2 = aval = (char **) zhalloc(sizeof(char *) * (arrlen(aval)+1));

	    while (*ap)
		*ap2++ = casemodify(*ap++, casmod);
	    *ap2++ = NULL;
	} else {
	    val = casemodify(val, casmod);
	}
    }
    /*
     * Perform prompt-style modifications.
     */
    if (presc) {
	int ops = opts[PROMPTSUBST], opb = opts[PROMPTBANG];
	int opp = opts[PROMPTPERCENT];

	if (presc < 2) {
	    opts[PROMPTPERCENT] = 1;
	    opts[PROMPTSUBST] = opts[PROMPTBANG] = 0;
	}
	/*
	 * TODO:  It would be really quite nice to abstract the
	 * isarr and !issarr code into a function which gets
	 * passed a pointer to a function with the effect of
	 * the promptexpand bit.  Then we could use this for
	 * a lot of stuff and bury val/aval/isarr inside a structure
	 * which gets passed to it.
	 */
	if (isarr) {
	    char **ap;

	    if (!copied)
		aval = arrdup(aval), copied = 1;
	    ap = aval;
	    for (; *ap; ap++) {
		char *tmps;
		untokenize(*ap);
		tmps = promptexpand(*ap, 0, NULL, NULL, NULL);
		*ap = dupstring(tmps);
		free(tmps);
	    }
	} else {
	    char *tmps;
	    if (!copied)
		val = dupstring(val), copied = 1;
	    untokenize(val);
	    tmps = promptexpand(val, 0, NULL, NULL, NULL);
	    val = dupstring(tmps);
	    free(tmps);
	}
	opts[PROMPTSUBST] = ops;
	opts[PROMPTBANG] = opb;
	opts[PROMPTPERCENT] = opp;
    }
    /*
     * One of the possible set of quotes to apply, depending on
     * the repetitions of the (q) flag.
     */
    if (quotemod) {
	if (quotetype > QT_DOLLARS)
	    quotetype = QT_DOLLARS;
	if (isarr) {
	    char **ap;

	    if (!copied)
		aval = arrdup(aval), copied = 1;
	    ap = aval;

	    if (quotemod > 0) {
		if (quotetype > QT_BACKSLASH) {
		    int sl;
		    char *tmp;

		    for (; *ap; ap++) {
			int pre = quotetype != QT_DOLLARS ? 1 : 2;
			tmp = quotestring(*ap, NULL, quotetype);
			sl = strlen(tmp);
			*ap = (char *) zhalloc(pre + sl + 2);
			strcpy((*ap) + pre, tmp);
			ap[0][pre - 1] = ap[0][pre + sl] =
			    (quotetype != QT_DOUBLE ? '\'' : '"');
			ap[0][pre + sl + 1] = '\0';
			if (quotetype == QT_DOLLARS)
			  ap[0][0] = '$';
		    }
		} else
		    for (; *ap; ap++)
			*ap = quotestring(*ap, NULL, QT_BACKSLASH);
	    } else {
		int one = noerrs, oef = errflag, haserr = 0;

		if (!quoteerr)
		    noerrs = 1;
		for (; *ap; ap++) {
		    haserr |= parse_subst_string(*ap);
		    remnulargs(*ap);
		    untokenize(*ap);
		}
		noerrs = one;
		if (!quoteerr)
		    errflag = oef;
		else if (haserr || errflag) {
		    zerr("parse error in parameter value");
		    return NULL;
		}
	    }
	} else {
	    if (!copied)
		val = dupstring(val), copied = 1;
	    if (quotemod > 0) {
		if (quotetype > QT_BACKSLASH) {
		    int pre = quotetype != QT_DOLLARS ? 1 : 2;
		    int sl;
		    char *tmp;
		    tmp = quotestring(val, NULL, quotetype);
		    sl = strlen(tmp);
		    val = (char *) zhalloc(pre + sl + 2);
		    strcpy(val + pre, tmp);
		    val[pre - 1] = val[pre + sl] =
			(quotetype != QT_DOUBLE ? '\'' : '"');
		    val[pre + sl + 1] = '\0';
		    if (quotetype == QT_DOLLARS)
		      val[0] = '$';
		} else
		    val = quotestring(val, NULL, QT_BACKSLASH);
	    } else {
		int one = noerrs, oef = errflag, haserr;

		if (!quoteerr)
		    noerrs = 1;
		haserr = parse_subst_string(val);
		noerrs = one;
		if (!quoteerr)
		    errflag = oef;
		else if (haserr || errflag) {
		    zerr("parse error in parameter value");
		    return NULL;
		}
		remnulargs(val);
		untokenize(val);
	    }
	}
    }
    /*
     * Transform special characters in the string to make them
     * printable.
     */
    if (visiblemod) {
	if (isarr) {
	    char **ap;
	    if (!copied)
		aval = arrdup(aval), copied = 1;
	    for (ap = aval; *ap; ap++)
		*ap = nicedupstring(*ap);
	} else {
	    if (!copied)
		val = dupstring(val), copied = 1;
	    val = nicedupstring(val);
	}
    }
    /*
     * Nothing particularly to do with SH_WORD_SPLIT --- this
     * performs lexical splitting on a string as specified by
     * the (z) flag.
     */
    if (shsplit) {
	LinkList list = NULL;

	if (isarr) {
	    char **ap;
	    for (ap = aval; *ap; ap++)
		list = bufferwords(list, *ap, NULL);
	    isarr = 0;
	} else
	    list = bufferwords(NULL, val, NULL);

	if (!list || !firstnode(list))
	    val = dupstring("");
	else if (!nextnode(firstnode(list)))
	    val = getdata(firstnode(list));
	else {
	    aval = hlinklist2array(list, 0);
	    isarr = nojoin ? 1 : 2;
	    l->list.flags |= LF_ARRAY;
	}
	copied = 1;
    }
    /*
     * TODO: hmm.  At this point we have to be on our toes about
     * whether we're putting stuff into a line or not, i.e.
     * we don't want to do this from a recursive call.
     * Rather than passing back flags in a non-trivial way, maybe
     * we could decide on the basis of flags passed down to us.
     *
     * This is the ideal place to do any last-minute conversion from
     * array to strings.  However, given all the transformations we've
     * already done, probably if it's going to be done it will already
     * have been.  (I'd really like to keep everying in aval or
     * equivalent and only locally decide if we need to treat it
     * as a scalar.)
     */
    if (isarr) {
	char *x;
	char *y;
	int xlen;
	int i;
	LinkNode on = n;

	/* Handle the (u) flag; we need this before the next test */
	if (unique) {
	    if(!copied)
		aval = arrdup(aval);

	    i = arrlen(aval);
	    if (i > 1)
		zhuniqarray(aval);
	}
	if ((!aval[0] || !aval[1]) && !plan9) {
	    /*
	     * Empty array or single element.  Currently you only
	     * get a single element array at this point from the
	     * unique expansion above. but we can potentially
	     * have other reasons.
	     *
	     * The following test removes the markers
	     * from surrounding double quotes, but I don't know why
	     * that's necessary.
	     */
	    int vallen;
	    if (aptr > (char *) getdata(n) &&
		aptr[-1] == Dnull && *fstr == Dnull)
		*--aptr = '\0', fstr++;
	    vallen = aval[0] ? strlen(aval[0]) : 0;
	    y = (char *) hcalloc((aptr - ostr) + vallen + strlen(fstr) + 1);
	    strcpy(y, ostr);
	    *str = y + (aptr - ostr);
	    if (vallen)
	    {
		strcpy(*str, aval[0]);
		*str += vallen;
	    }
	    strcpy(*str, fstr);
	    setdata(n, y);
	    return n;
	}
	/* Handle (o) and (O) and their variants */
	if (sortit != SORTIT_ANYOLDHOW) {
	    if (!copied)
		aval = arrdup(aval);
	    if (indord) {
		if (sortit & SORTIT_BACKWARDS) {
		    char *copy;
		    char **end = aval + arrlen(aval) - 1, **start = aval;

		    /* reverse the array */
		    while (start < end) {
			copy = *end;
			*end-- = *start;
			*start++ = copy;
		    }
		}
	    } else {
		/*
		 * HERE: we tested if the last element of the array
		 * was not a NULL string.  Why the last element?
		 * Why didn't we expect NULL strings to work?
		 * Was it just a clumsy way of testing whether there
		 * was enough in the array to sort?
		 */
		strmetasort(aval, sortit, NULL);
	    }
	}
	if (plan9) {
	    /* Handle RC_EXPAND_PARAM */
	    LinkNode tn;
	    local_list1(tl);

	    *--fstr = Marker;
	    init_list1(tl, fstr);
	    if (!eval && !stringsubst(&tl, firstnode(&tl), ssub, 0))
		return NULL;
	    *str = aptr;
	    tn = firstnode(&tl);
	    while ((x = *aval++)) {
		if (prenum || postnum)
		    x = dopadding(x, prenum, postnum, preone, postone,
				  premul, postmul
#ifdef MULTIBYTE_SUPPORT
				  , multi_width
#endif
			);
		if (eval && subst_parse_str(&x, (qt && !nojoin), quoteerr))
		    return NULL;
		xlen = strlen(x);
		for (tn = firstnode(&tl);
		     tn && *(y = (char *) getdata(tn)) == Marker;
		     incnode(tn)) {
		    strcatsub(&y, ostr, aptr, x, xlen, y + 1, globsubst,
			      copied);
		    if (qt && !*y && isarr != 2)
			y = dupstring(nulstring);
		    if (plan9)
			setdata(n, (void *) y), plan9 = 0;
		    else
			insertlinknode(l, n, (void *) y), incnode(n);
		}
	    }
	    for (; tn; incnode(tn)) {
		y = (char *) getdata(tn);
		if (*y == Marker)
		    continue;
		if (qt && !*y && isarr != 2)
		    y = dupstring(nulstring);
		if (plan9)
		    setdata(n, (void *) y), plan9 = 0;
		else
		    insertlinknode(l, n, (void *) y), incnode(n);
	    }
	    if (plan9) {
		uremnode(l, n);
		return n;
	    }
	} else {
	    /*
	     * Not RC_EXPAND_PARAM: simply join the first and
	     * last values.
	     * TODO: how about removing the restriction that
	     * aval[1] is non-NULL to promote consistency?, or
	     * simply changing the test so that we drop into
	     * the scalar branch, instead of tricking isarr?
	     */
	    x = aval[0];
	    if (prenum || postnum)
		x = dopadding(x, prenum, postnum, preone, postone,
			      premul, postmul
#ifdef MULTIBYTE_SUPPORT
			      , multi_width
#endif
		    );
	    if (eval && subst_parse_str(&x, (qt && !nojoin), quoteerr))
		return NULL;
	    xlen = strlen(x);
	    strcatsub(&y, ostr, aptr, x, xlen, NULL, globsubst, copied);
	    if (qt && !*y && isarr != 2)
		y = dupstring(nulstring);
	    setdata(n, (void *) y);

	    i = 1;
	    /* aval[1] is non-null here */
	    while (aval[i + 1]) {
		x = aval[i++];
		if (prenum || postnum)
		    x = dopadding(x, prenum, postnum, preone, postone,
				  premul, postmul
#ifdef MULTIBYTE_SUPPORT
				  , multi_width
#endif
			);
		if (eval && subst_parse_str(&x, (qt && !nojoin), quoteerr))
		    return NULL;
		if (qt && !*x && isarr != 2)
		    y = dupstring(nulstring);
		else {
		    y = dupstring(x);
		    if (globsubst)
			shtokenize(y);
		}
		insertlinknode(l, n, (void *) y), incnode(n);
	    }

	    x = aval[i];
	    if (prenum || postnum)
		x = dopadding(x, prenum, postnum, preone, postone,
			      premul, postmul
#ifdef MULTIBYTE_SUPPORT
			      , multi_width
#endif
		    );
	    if (eval && subst_parse_str(&x, (qt && !nojoin), quoteerr))
		return NULL;
	    xlen = strlen(x);
	    *str = strcatsub(&y, aptr, aptr, x, xlen, fstr, globsubst, copied);
	    if (qt && !*y && isarr != 2)
		y = dupstring(nulstring);
	    insertlinknode(l, n, (void *) y), incnode(n);
	}
	if (eval)
	    n = on;
    } else {
	/*
	 * Scalar value.  Handle last minute transformations
	 * such as left- or right-padding and the (e) flag to
	 * revaluate the result.
	 */
	int xlen;
	char *x;
	char *y;

	x = val;
	if (prenum || postnum)
	    x = dopadding(x, prenum, postnum, preone, postone,
			  premul, postmul
#ifdef MULTIBYTE_SUPPORT
			  , multi_width
#endif
		);
	if (eval && subst_parse_str(&x, (qt && !nojoin), quoteerr))
	    return NULL;
	xlen = strlen(x);
	*str = strcatsub(&y, ostr, aptr, x, xlen, fstr, globsubst, copied);
	if (qt && !*y)
	    y = dupstring(nulstring);
	setdata(n, (void *) y);
    }
    if (eval)
	*str = (char *) getdata(n);

    return n;
}

/*
 * Arithmetic substitution: `a' is the string to be evaluated, `bptr'
 * points to the beginning of the string containing it.  The tail of
 * the string is given by `rest'. *bptr is modified with the substituted
 * string. The function returns a pointer to the tail in the substituted
 * string.
 */

/**/
static char *
arithsubst(char *a, char **bptr, char *rest)
{
    char *s = *bptr, *t;
    char buf[BDIGBUFSIZE], *b = buf;
    mnumber v;

    singsub(&a);
    v = matheval(a);
    if ((v.type & MN_FLOAT) && !outputradix)
	b = convfloat(v.u.d, 0, 0, NULL);
    else {
	if (v.type & MN_FLOAT)
	    v.u.l = (zlong) v.u.d;
	convbase(buf, v.u.l, outputradix);
    }
    t = *bptr = (char *) hcalloc(strlen(*bptr) + strlen(b) + 
				 strlen(rest) + 1);
    t--;
    while ((*++t = *s++));
    t--;
    while ((*++t = *b++));
    strcat(t, rest);
    return t;
}

/**/
void
modify(char **str, char **ptr)
{
    char *ptr1, *ptr2, *ptr3, *lptr, c, *test, *sep, *t, *tt, tc, *e;
    char *copy, *all, *tmp, sav, sav1, *ptr1end;
    int gbal, wall, rec, al, nl, charlen, dellen;
    convchar_t del;

    test = NULL;

    if (**ptr == ':')
	*str = dupstring(*str);

    while (**ptr == ':') {
	lptr = *ptr;
	(*ptr)++;
	wall = gbal = 0;
	rec = 1;
	c = '\0';
	sep = NULL;

	for (; !c && **ptr;) {
	    switch (**ptr) {
            case 'a':
            case 'A':
	    case 'c':
	    case 'h':
	    case 'r':
	    case 'e':
	    case 't':
	    case 'l':
	    case 'u':
	    case 'q':
	    case 'Q':
		c = **ptr;
		break;

	    case 's':
		c = **ptr;
		(*ptr)++;
		ptr1 = *ptr;
		MB_METACHARINIT();
		charlen = MB_METACHARLENCONV(ptr1, &del);
#ifdef MULTIBYTE_SUPPORT
		if (del == WEOF)
		    del = (wint_t)((*ptr1 == Meta) ? ptr1[1] ^ 32 : *ptr1);
#endif
		ptr1 += charlen;
		for (ptr2 = ptr1, charlen = 0; *ptr2; ptr2 += charlen) {
		    convchar_t del2;
		    charlen = MB_METACHARLENCONV(ptr2, &del2);
#ifdef MULTIBYTE_SUPPORT
		    if (del2 == WEOF)
			del2 = (wint_t)((*ptr2 == Meta) ?
					ptr2[1] ^ 32 : *ptr2);
#endif
		    if (del2 == del)
			break;
		}
		if (!*ptr2) {
		    zerr("bad substitution");
		    return;
		}
		ptr1end = ptr2;
		ptr2 += charlen;
		sav1 = *ptr1end;
		*ptr1end = '\0';
		for (ptr3 = ptr2, charlen = 0; *ptr3; ptr3 += charlen) {
		    convchar_t del3;
		    charlen = MB_METACHARLENCONV(ptr3, &del3);
#ifdef MULTIBYTE_SUPPORT
		    if (del3 == WEOF)
			del3 = (wint_t)((*ptr3 == Meta) ?
					ptr3[1] ^ 32 : *ptr3);
#endif
		    if (del3 == del)
			break;
		}
		sav = *ptr3;
		*ptr3 = '\0';
		if (*ptr1) {
		    zsfree(hsubl);
		    hsubl = ztrdup(ptr1);
 		}
		if (!hsubl) {
		    zerr("no previous substitution");
		    return;
		}
		zsfree(hsubr);
		for (tt = hsubl; *tt; tt++)
		    if (inull(*tt) && *tt != Bnullkeep)
			chuck(tt--);
		if (!isset(HISTSUBSTPATTERN))
		    untokenize(hsubl);
		for (tt = hsubr = ztrdup(ptr2); *tt; tt++)
		    if (inull(*tt) && *tt != Bnullkeep)
			chuck(tt--);
		*ptr1end = sav1;
		*ptr3 = sav;
		*ptr = ptr3 - 1;
		if (*ptr3) {
		    /* Final terminator is optional. */
		    *ptr += charlen;
		}
		break;

	    case '&':
		c = 's';
		break;

	    case 'g':
		(*ptr)++;
		gbal = 1;
		break;

	    case 'w':
		wall = 1;
		(*ptr)++;
		break;
	    case 'W':
		wall = 1;
		(*ptr)++;
		ptr1 = get_strarg(ptr2 = *ptr, &charlen);
		if ((sav = *ptr1))
		    *ptr1 = '\0';
		sep = dupstring(ptr2 + charlen);
		if (sav)
		    *ptr1 = sav;
		*ptr = ptr1 + charlen;
		c = '\0';
		break;

	    case 'f':
		rec = -1;
		(*ptr)++;
		break;
	    case 'F':
		(*ptr)++;
		rec = get_intarg(ptr, &dellen);
		break;
	    default:
		*ptr = lptr;
		return;
	    }
	}
	(*ptr)++;
	if (!c) {
	    *ptr = lptr;
	    return;
	}
	if (rec < 0)
	    test = dupstring(*str);

	while (rec--) {
	    if (wall) {
		al = 0;
		all = NULL;
		for (t = e = *str; (tt = findword(&e, sep));) {
		    tc = *e;
		    *e = '\0';
		    if (c != 'l' && c != 'u')
			copy = dupstring(tt);
		    *e = tc;
		    switch (c) {
                    case 'a':
			chabspath(&copy);
			break;
		    case 'A':
			chrealpath(&copy);
			break;
		    case 'c':
		    {
			char *copy2 = equalsubstr(copy, 0, 0);
			if (copy2)
			    copy = copy2;
			break;
		    }
		    case 'h':
			remtpath(&copy);
			break;
		    case 'r':
			remtext(&copy);
			break;
		    case 'e':
			rembutext(&copy);
			break;
		    case 't':
			remlpaths(&copy);
			break;
		    case 'l':
			copy = casemodify(tt, CASMOD_LOWER);
			break;
		    case 'u':
			copy = casemodify(tt, CASMOD_UPPER);
			break;
		    case 's':
			if (hsubl && hsubr)
			    subst(&copy, hsubl, hsubr, gbal);
			break;
		    case 'q':
			copy = quotestring(copy, NULL, QT_BACKSLASH);
			break;
		    case 'Q':
			{
			    int one = noerrs, oef = errflag;

			    noerrs = 1;
			    parse_subst_string(copy);
			    noerrs = one;
			    errflag = oef;
			    remnulargs(copy);
			    untokenize(copy);
			}
			break;
		    }
		    tc = *tt;
		    *tt = '\0';
		    nl = al + strlen(t) + strlen(copy);
		    ptr1 = tmp = (char *)zhalloc(nl + 1);
		    if (all)
			for (ptr2 = all; *ptr2;)
			    *ptr1++ = *ptr2++;
		    for (ptr2 = t; *ptr2;)
			*ptr1++ = *ptr2++;
		    *tt = tc;
		    for (ptr2 = copy; *ptr2;)
			*ptr1++ = *ptr2++;
		    *ptr1 = '\0';
		    al = nl;
		    all = tmp;
		    t = e;
		}
		*str = all;

	    } else {
		switch (c) {
		case 'a':
		    chabspath(str);
		    break;
		case 'A':
		    chrealpath(str);
		    break;
		case 'c':
		{
		    char *copy2 = equalsubstr(*str, 0, 0);
		    if (copy2)
			*str = copy2;
		    break;
		}
		case 'h':
		    remtpath(str);
		    break;
		case 'r':
		    remtext(str);
		    break;
		case 'e':
		    rembutext(str);
		    break;
		case 't':
		    remlpaths(str);
		    break;
		case 'l':
		    *str = casemodify(*str, CASMOD_LOWER);
		    break;
		case 'u':
		    *str = casemodify(*str, CASMOD_UPPER);
		    break;
		case 's':
		    if (hsubl && hsubr)
			subst(str, hsubl, hsubr, gbal);
		    break;
		case 'q':
		    *str = quotestring(*str, NULL, QT_BACKSLASH);
		    break;
		case 'Q':
		    {
			int one = noerrs, oef = errflag;

			noerrs = 1;
			parse_subst_string(*str);
			noerrs = one;
			errflag = oef;
			remnulargs(*str);
			untokenize(*str);
		    }
		    break;
		}
	    }
	    if (rec < 0) {
		if (!strcmp(test, *str))
		    rec = 0;
		else
		    test = dupstring(*str);
	    }
	}
    }
}

/* get a directory stack entry */

/**/
static char *
dstackent(char ch, int val)
{
    int backwards;
    LinkNode end=(LinkNode)dirstack, n;

    backwards = ch == (isset(PUSHDMINUS) ? '+' : '-');
    if(!backwards && !val--)
	return pwd;
    if (backwards)
	for (n=lastnode(dirstack); n != end && val; val--, n=prevnode(n));
    else
	for (end=NULL, n=firstnode(dirstack); n && val; val--, n=nextnode(n));
    if (n == end) {
	if (backwards && !val)
	    return pwd;
	if (isset(NOMATCH))
	    zerr("not enough directory stack entries.");
	return NULL;
    }
    return (char *)getdata(n);
}
