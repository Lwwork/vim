/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * quickfix.c: functions for quickfix mode, using a file with error messages
 */

#include "vim.h"

#if defined(FEAT_QUICKFIX) || defined(PROTO)

struct dir_stack_T
{
    struct dir_stack_T	*next;
    char_u		*dirname;
};

/*
 * For each error the next struct is allocated and linked in a list.
 */
typedef struct qfline_S qfline_T;
struct qfline_S
{
    qfline_T	*qf_next;	/* pointer to next error in the list */
    qfline_T	*qf_prev;	/* pointer to previous error in the list */
    linenr_T	qf_lnum;	/* line number where the error occurred */
    int		qf_fnum;	/* file number for the line */
    int		qf_col;		/* column where the error occurred */
    int		qf_nr;		/* error number */
    char_u	*qf_module;	/* module name for this error */
    char_u	*qf_pattern;	/* search pattern for the error */
    char_u	*qf_text;	/* description of the error */
    char_u	qf_viscol;	/* set to TRUE if qf_col is screen column */
    char_u	qf_cleared;	/* set to TRUE if line has been deleted */
    char_u	qf_type;	/* type of the error (mostly 'E'); 1 for
				   :helpgrep */
    char_u	qf_valid;	/* valid error message detected */
};

/*
 * There is a stack of error lists.
 */
#define LISTCOUNT   10
#define INVALID_QFIDX (-1)

/*
 * Quickfix/Location list definition
 * Contains a list of entries (qfline_T). qf_start points to the first entry
 * and qf_last points to the last entry. qf_count contains the list size.
 *
 * Usually the list contains one or more entries. But an empty list can be
 * created using setqflist()/setloclist() with a title and/or user context
 * information and entries can be added later using setqflist()/setloclist().
 */
typedef struct qf_list_S
{
    int_u	qf_id;		/* Unique identifier for this list */
    qfline_T	*qf_start;	/* pointer to the first error */
    qfline_T	*qf_last;	/* pointer to the last error */
    qfline_T	*qf_ptr;	/* pointer to the current error */
    int		qf_count;	/* number of errors (0 means empty list) */
    int		qf_index;	/* current index in the error list */
    int		qf_nonevalid;	/* TRUE if not a single valid entry found */
    char_u	*qf_title;	/* title derived from the command that created
				 * the error list or set by setqflist */
    typval_T	*qf_ctx;	/* context set by setqflist/setloclist */

    struct dir_stack_T	*qf_dir_stack;
    char_u		*qf_directory;
    struct dir_stack_T	*qf_file_stack;
    char_u		*qf_currfile;
    int			qf_multiline;
    int			qf_multiignore;
    int			qf_multiscan;
    long		qf_changedtick;
} qf_list_T;

/*
 * Quickfix/Location list stack definition
 * Contains a list of quickfix/location lists (qf_list_T)
 */
struct qf_info_S
{
    /*
     * Count of references to this list. Used only for location lists.
     * When a location list window reference this list, qf_refcount
     * will be 2. Otherwise, qf_refcount will be 1. When qf_refcount
     * reaches 0, the list is freed.
     */
    int		qf_refcount;
    int		qf_listcount;	    /* current number of lists */
    int		qf_curlist;	    /* current error list */
    qf_list_T	qf_lists[LISTCOUNT];
};

static qf_info_T ql_info;	/* global quickfix list */
static int_u last_qf_id = 0;	/* Last used quickfix list id */

#define FMT_PATTERNS 11		/* maximum number of % recognized */

/*
 * Structure used to hold the info of one part of 'errorformat'
 */
typedef struct efm_S efm_T;
struct efm_S
{
    regprog_T	    *prog;	/* pre-formatted part of 'errorformat' */
    efm_T	    *next;	/* pointer to next (NULL if last) */
    char_u	    addr[FMT_PATTERNS]; /* indices of used % patterns */
    char_u	    prefix;	/* prefix of this format line: */
				/*   'D' enter directory */
				/*   'X' leave directory */
				/*   'A' start of multi-line message */
				/*   'E' error message */
				/*   'W' warning message */
				/*   'I' informational message */
				/*   'C' continuation line */
				/*   'Z' end of multi-line message */
				/*   'G' general, unspecific message */
				/*   'P' push file (partial) message */
				/*   'Q' pop/quit file (partial) message */
				/*   'O' overread (partial) message */
    char_u	    flags;	/* additional flags given in prefix */
				/*   '-' do not include this line */
				/*   '+' include whole line in message */
    int		    conthere;	/* %> used */
};

static efm_T	*fmt_start = NULL; /* cached across qf_parse_line() calls */

static int	qf_init_ext(qf_info_T *qi, int qf_idx, char_u *efile, buf_T *buf, typval_T *tv, char_u *errorformat, int newlist, linenr_T lnumfirst, linenr_T lnumlast, char_u *qf_title, char_u *enc);
static void	qf_new_list(qf_info_T *qi, char_u *qf_title);
static int	qf_add_entry(qf_info_T *qi, int qf_idx, char_u *dir, char_u *fname, char_u *module, int bufnum, char_u *mesg, long lnum, int col, int vis_col, char_u *pattern, int nr, int type, int valid);
static qf_info_T *ll_new_list(void);
static void	qf_free(qf_info_T *qi, int idx);
static char_u	*qf_types(int, int);
static int	qf_get_fnum(qf_info_T *qi, int qf_idx, char_u *, char_u *);
static char_u	*qf_push_dir(char_u *, struct dir_stack_T **, int is_file_stack);
static char_u	*qf_pop_dir(struct dir_stack_T **);
static char_u	*qf_guess_filepath(qf_info_T *qi, int qf_idx, char_u *);
static void	qf_fmt_text(char_u *text, char_u *buf, int bufsize);
static int	qf_win_pos_update(qf_info_T *qi, int old_qf_index);
static win_T	*qf_find_win(qf_info_T *qi);
static buf_T	*qf_find_buf(qf_info_T *qi);
static void	qf_update_buffer(qf_info_T *qi, qfline_T *old_last);
static void	qf_set_title_var(qf_info_T *qi);
static void	qf_fill_buffer(qf_info_T *qi, buf_T *buf, qfline_T *old_last);
static char_u	*get_mef_name(void);
static buf_T	*load_dummy_buffer(char_u *fname, char_u *dirname_start, char_u *resulting_dir);
static void	wipe_dummy_buffer(buf_T *buf, char_u *dirname_start);
static void	unload_dummy_buffer(buf_T *buf, char_u *dirname_start);
static qf_info_T *ll_get_or_alloc_list(win_T *);

/* Quickfix window check helper macro */
#define IS_QF_WINDOW(wp) (bt_quickfix(wp->w_buffer) && wp->w_llist_ref == NULL)
/* Location list window check helper macro */
#define IS_LL_WINDOW(wp) (bt_quickfix(wp->w_buffer) && wp->w_llist_ref != NULL)
/*
 * Return location list for window 'wp'
 * For location list window, return the referenced location list
 */
#define GET_LOC_LIST(wp) (IS_LL_WINDOW(wp) ? wp->w_llist_ref : wp->w_llist)

/*
 * Looking up a buffer can be slow if there are many.  Remember the last one
 * to make this a lot faster if there are multiple matches in the same file.
 */
static char_u   *qf_last_bufname = NULL;
static bufref_T  qf_last_bufref = {NULL, 0, 0};

static char	*e_loc_list_changed =
				 N_("E926: Current location list was changed");

/*
 * Read the errorfile "efile" into memory, line by line, building the error
 * list. Set the error list's title to qf_title.
 * Return -1 for error, number of errors for success.
 */
    int
qf_init(win_T	    *wp,
	char_u	    *efile,
	char_u	    *errorformat,
	int	    newlist,		/* TRUE: start a new error list */
	char_u	    *qf_title,
	char_u	    *enc)
{
    qf_info_T	    *qi = &ql_info;

    if (wp != NULL)
    {
	qi = ll_get_or_alloc_list(wp);
	if (qi == NULL)
	    return FAIL;
    }

    return qf_init_ext(qi, qi->qf_curlist, efile, curbuf, NULL, errorformat,
	    newlist, (linenr_T)0, (linenr_T)0, qf_title, enc);
}

/*
 * Maximum number of bytes allowed per line while reading a errorfile.
 */
#define LINE_MAXLEN 4096

static struct fmtpattern
{
    char_u	convchar;
    char	*pattern;
} fmt_pat[FMT_PATTERNS] =
    {
	{'f', ".\\+"},	    /* only used when at end */
	{'n', "\\d\\+"},
	{'l', "\\d\\+"},
	{'c', "\\d\\+"},
	{'t', "."},
	{'m', ".\\+"},
	{'r', ".*"},
	{'p', "[- 	.]*"},
	{'v', "\\d\\+"},
	{'s', ".\\+"},
	{'o', ".\\+"}
    };

/*
 * Converts a 'errorformat' string to regular expression pattern
 */
    static int
efm_to_regpat(
	char_u	*efm,
	int		len,
	efm_T	*fmt_ptr,
	char_u	*regpat,
	char_u	*errmsg)
{
    char_u	*ptr;
    char_u	*efmp;
    char_u	*srcptr;
    int		round;
    int		idx = 0;

    /*
     * Build regexp pattern from current 'errorformat' option
     */
    ptr = regpat;
    *ptr++ = '^';
    round = 0;
    for (efmp = efm; efmp < efm + len; ++efmp)
    {
	if (*efmp == '%')
	{
	    ++efmp;
	    for (idx = 0; idx < FMT_PATTERNS; ++idx)
		if (fmt_pat[idx].convchar == *efmp)
		    break;
	    if (idx < FMT_PATTERNS)
	    {
		if (fmt_ptr->addr[idx])
		{
		    sprintf((char *)errmsg,
			    _("E372: Too many %%%c in format string"), *efmp);
		    EMSG(errmsg);
		    return -1;
		}
		if ((idx
			    && idx < 6
			    && vim_strchr((char_u *)"DXOPQ",
				fmt_ptr->prefix) != NULL)
			|| (idx == 6
			    && vim_strchr((char_u *)"OPQ",
				fmt_ptr->prefix) == NULL))
		{
		    sprintf((char *)errmsg,
			    _("E373: Unexpected %%%c in format string"), *efmp);
		    EMSG(errmsg);
		    return -1;
		}
		fmt_ptr->addr[idx] = (char_u)++round;
		*ptr++ = '\\';
		*ptr++ = '(';
#ifdef BACKSLASH_IN_FILENAME
		if (*efmp == 'f')
		{
		    /* Also match "c:" in the file name, even when
		     * checking for a colon next: "%f:".
		     * "\%(\a:\)\=" */
		    STRCPY(ptr, "\\%(\\a:\\)\\=");
		    ptr += 10;
		}
#endif
		if (*efmp == 'f' && efmp[1] != NUL)
		{
		    if (efmp[1] != '\\' && efmp[1] != '%')
		    {
			/* A file name may contain spaces, but this isn't
			 * in "\f".  For "%f:%l:%m" there may be a ":" in
			 * the file name.  Use ".\{-1,}x" instead (x is
			 * the next character), the requirement that :999:
			 * follows should work. */
			STRCPY(ptr, ".\\{-1,}");
			ptr += 7;
		    }
		    else
		    {
			/* File name followed by '\\' or '%': include as
			 * many file name chars as possible. */
			STRCPY(ptr, "\\f\\+");
			ptr += 4;
		    }
		}
		else
		{
		    srcptr = (char_u *)fmt_pat[idx].pattern;
		    while ((*ptr = *srcptr++) != NUL)
			++ptr;
		}
		*ptr++ = '\\';
		*ptr++ = ')';
	    }
	    else if (*efmp == '*')
	    {
		if (*++efmp == '[' || *efmp == '\\')
		{
		    if ((*ptr++ = *efmp) == '[')	/* %*[^a-z0-9] etc. */
		    {
			if (efmp[1] == '^')
			    *ptr++ = *++efmp;
			if (efmp < efm + len)
			{
			    *ptr++ = *++efmp;	    /* could be ']' */
			    while (efmp < efm + len
				    && (*ptr++ = *++efmp) != ']')
				/* skip */;
			    if (efmp == efm + len)
			    {
				EMSG(_("E374: Missing ] in format string"));
				return -1;
			    }
			}
		    }
		    else if (efmp < efm + len)	/* %*\D, %*\s etc. */
			*ptr++ = *++efmp;
		    *ptr++ = '\\';
		    *ptr++ = '+';
		}
		else
		{
		    /* TODO: scanf()-like: %*ud, %*3c, %*f, ... ? */
		    sprintf((char *)errmsg,
			    _("E375: Unsupported %%%c in format string"), *efmp);
		    EMSG(errmsg);
		    return -1;
		}
	    }
	    else if (vim_strchr((char_u *)"%\\.^$~[", *efmp) != NULL)
		*ptr++ = *efmp;		/* regexp magic characters */
	    else if (*efmp == '#')
		*ptr++ = '*';
	    else if (*efmp == '>')
		fmt_ptr->conthere = TRUE;
	    else if (efmp == efm + 1)		/* analyse prefix */
	    {
		if (vim_strchr((char_u *)"+-", *efmp) != NULL)
		    fmt_ptr->flags = *efmp++;
		if (vim_strchr((char_u *)"DXAEWICZGOPQ", *efmp) != NULL)
		    fmt_ptr->prefix = *efmp;
		else
		{
		    sprintf((char *)errmsg,
			    _("E376: Invalid %%%c in format string prefix"), *efmp);
		    EMSG(errmsg);
		    return -1;
		}
	    }
	    else
	    {
		sprintf((char *)errmsg,
			_("E377: Invalid %%%c in format string"), *efmp);
		EMSG(errmsg);
		return -1;
	    }
	}
	else			/* copy normal character */
	{
	    if (*efmp == '\\' && efmp + 1 < efm + len)
		++efmp;
	    else if (vim_strchr((char_u *)".*^$~[", *efmp) != NULL)
		*ptr++ = '\\';	/* escape regexp atoms */
	    if (*efmp)
		*ptr++ = *efmp;
	}
    }
    *ptr++ = '$';
    *ptr = NUL;

    return 0;
}

    static void
free_efm_list(efm_T **efm_first)
{
    efm_T *efm_ptr;

    for (efm_ptr = *efm_first; efm_ptr != NULL; efm_ptr = *efm_first)
    {
	*efm_first = efm_ptr->next;
	vim_regfree(efm_ptr->prog);
	vim_free(efm_ptr);
    }
    fmt_start = NULL;
}

/* Parse 'errorformat' option */
    static efm_T *
parse_efm_option(char_u *efm)
{
    char_u	*errmsg = NULL;
    int		errmsglen;
    efm_T	*fmt_ptr = NULL;
    efm_T	*fmt_first = NULL;
    efm_T	*fmt_last = NULL;
    char_u	*fmtstr = NULL;
    int		len;
    int		i;
    int		round;

    errmsglen = CMDBUFFSIZE + 1;
    errmsg = alloc_id(errmsglen, aid_qf_errmsg);
    if (errmsg == NULL)
	goto parse_efm_end;

    /*
     * Each part of the format string is copied and modified from errorformat
     * to regex prog.  Only a few % characters are allowed.
     */

    /*
     * Get some space to modify the format string into.
     */
    i = (FMT_PATTERNS * 3) + ((int)STRLEN(efm) << 2);
    for (round = FMT_PATTERNS; round > 0; )
	i += (int)STRLEN(fmt_pat[--round].pattern);
#ifdef BACKSLASH_IN_FILENAME
    i += 12; /* "%f" can become twelve chars longer (see efm_to_regpat) */
#else
    i += 2; /* "%f" can become two chars longer */
#endif
    if ((fmtstr = alloc(i)) == NULL)
	goto parse_efm_error;

    while (efm[0] != NUL)
    {
	/*
	 * Allocate a new eformat structure and put it at the end of the list
	 */
	fmt_ptr = (efm_T *)alloc_clear((unsigned)sizeof(efm_T));
	if (fmt_ptr == NULL)
	    goto parse_efm_error;
	if (fmt_first == NULL)	    /* first one */
	    fmt_first = fmt_ptr;
	else
	    fmt_last->next = fmt_ptr;
	fmt_last = fmt_ptr;

	/*
	 * Isolate one part in the 'errorformat' option
	 */
	for (len = 0; efm[len] != NUL && efm[len] != ','; ++len)
	    if (efm[len] == '\\' && efm[len + 1] != NUL)
		++len;

	if (efm_to_regpat(efm, len, fmt_ptr, fmtstr, errmsg) == -1)
	    goto parse_efm_error;
	if ((fmt_ptr->prog = vim_regcomp(fmtstr, RE_MAGIC + RE_STRING)) == NULL)
	    goto parse_efm_error;
	/*
	 * Advance to next part
	 */
	efm = skip_to_option_part(efm + len);	/* skip comma and spaces */
    }

    if (fmt_first == NULL)	/* nothing found */
	EMSG(_("E378: 'errorformat' contains no pattern"));

    goto parse_efm_end;

parse_efm_error:
    free_efm_list(&fmt_first);

parse_efm_end:
    vim_free(fmtstr);
    vim_free(errmsg);

    return fmt_first;
}

enum {
    QF_FAIL = 0,
    QF_OK = 1,
    QF_END_OF_INPUT = 2,
    QF_NOMEM = 3,
    QF_IGNORE_LINE = 4,
    QF_MULTISCAN = 5,
};

typedef struct {
    char_u	*linebuf;
    int		linelen;
    char_u	*growbuf;
    int		growbufsiz;
    FILE	*fd;
    typval_T	*tv;
    char_u	*p_str;
    listitem_T	*p_li;
    buf_T	*buf;
    linenr_T	buflnum;
    linenr_T	lnumlast;
    vimconv_T	vc;
} qfstate_T;

    static char_u *
qf_grow_linebuf(qfstate_T *state, int newsz)
{
    char_u	*p;

    /*
     * If the line exceeds LINE_MAXLEN exclude the last
     * byte since it's not a NL character.
     */
    state->linelen = newsz > LINE_MAXLEN ? LINE_MAXLEN - 1 : newsz;
    if (state->growbuf == NULL)
    {
	state->growbuf = alloc(state->linelen + 1);
	if (state->growbuf == NULL)
	    return NULL;
	state->growbufsiz = state->linelen;
    }
    else if (state->linelen > state->growbufsiz)
    {
	if ((p = vim_realloc(state->growbuf, state->linelen + 1)) == NULL)
	    return NULL;
	state->growbuf = p;
	state->growbufsiz = state->linelen;
    }
    return state->growbuf;
}

/*
 * Get the next string (separated by newline) from state->p_str.
 */
    static int
qf_get_next_str_line(qfstate_T *state)
{
    /* Get the next line from the supplied string */
    char_u	*p_str = state->p_str;
    char_u	*p;
    int		len;

    if (*p_str == NUL) /* Reached the end of the string */
	return QF_END_OF_INPUT;

    p = vim_strchr(p_str, '\n');
    if (p != NULL)
	len = (int)(p - p_str) + 1;
    else
	len = (int)STRLEN(p_str);

    if (len > IOSIZE - 2)
    {
	state->linebuf = qf_grow_linebuf(state, len);
	if (state->linebuf == NULL)
	    return QF_NOMEM;
    }
    else
    {
	state->linebuf = IObuff;
	state->linelen = len;
    }
    vim_strncpy(state->linebuf, p_str, state->linelen);

    /*
     * Increment using len in order to discard the rest of the
     * line if it exceeds LINE_MAXLEN.
     */
    p_str += len;
    state->p_str = p_str;

    return QF_OK;
}

/*
 * Get the next string from state->p_Li.
 */
    static int
qf_get_next_list_line(qfstate_T *state)
{
    listitem_T	*p_li = state->p_li;
    int		len;

    while (p_li != NULL
	    && (p_li->li_tv.v_type != VAR_STRING
		|| p_li->li_tv.vval.v_string == NULL))
	p_li = p_li->li_next;	/* Skip non-string items */

    if (p_li == NULL)		/* End of the list */
    {
	state->p_li = NULL;
	return QF_END_OF_INPUT;
    }

    len = (int)STRLEN(p_li->li_tv.vval.v_string);
    if (len > IOSIZE - 2)
    {
	state->linebuf = qf_grow_linebuf(state, len);
	if (state->linebuf == NULL)
	    return QF_NOMEM;
    }
    else
    {
	state->linebuf = IObuff;
	state->linelen = len;
    }

    vim_strncpy(state->linebuf, p_li->li_tv.vval.v_string, state->linelen);

    state->p_li = p_li->li_next;	/* next item */
    return QF_OK;
}

/*
 * Get the next string from state->buf.
 */
    static int
qf_get_next_buf_line(qfstate_T *state)
{
    char_u	*p_buf = NULL;
    int		len;

    /* Get the next line from the supplied buffer */
    if (state->buflnum > state->lnumlast)
	return QF_END_OF_INPUT;

    p_buf = ml_get_buf(state->buf, state->buflnum, FALSE);
    state->buflnum += 1;

    len = (int)STRLEN(p_buf);
    if (len > IOSIZE - 2)
    {
	state->linebuf = qf_grow_linebuf(state, len);
	if (state->linebuf == NULL)
	    return QF_NOMEM;
    }
    else
    {
	state->linebuf = IObuff;
	state->linelen = len;
    }
    vim_strncpy(state->linebuf, p_buf, state->linelen);

    return QF_OK;
}

/*
 * Get the next string from file state->fd.
 */
    static int
qf_get_next_file_line(qfstate_T *state)
{
    int	    discard;
    int	    growbuflen;

    if (fgets((char *)IObuff, IOSIZE, state->fd) == NULL)
	return QF_END_OF_INPUT;

    discard = FALSE;
    state->linelen = (int)STRLEN(IObuff);
    if (state->linelen == IOSIZE - 1 && !(IObuff[state->linelen - 1] == '\n'))
    {
	/*
	 * The current line exceeds IObuff, continue reading using
	 * growbuf until EOL or LINE_MAXLEN bytes is read.
	 */
	if (state->growbuf == NULL)
	{
	    state->growbufsiz = 2 * (IOSIZE - 1);
	    state->growbuf = alloc(state->growbufsiz);
	    if (state->growbuf == NULL)
		return QF_NOMEM;
	}

	/* Copy the read part of the line, excluding null-terminator */
	memcpy(state->growbuf, IObuff, IOSIZE - 1);
	growbuflen = state->linelen;

	for (;;)
	{
	    char_u	*p;

	    if (fgets((char *)state->growbuf + growbuflen,
			state->growbufsiz - growbuflen, state->fd) == NULL)
		break;
	    state->linelen = (int)STRLEN(state->growbuf + growbuflen);
	    growbuflen += state->linelen;
	    if ((state->growbuf)[growbuflen - 1] == '\n')
		break;
	    if (state->growbufsiz == LINE_MAXLEN)
	    {
		discard = TRUE;
		break;
	    }

	    state->growbufsiz = 2 * state->growbufsiz < LINE_MAXLEN
		? 2 * state->growbufsiz : LINE_MAXLEN;
	    if ((p = vim_realloc(state->growbuf, state->growbufsiz)) == NULL)
		return QF_NOMEM;
	    state->growbuf = p;
	}

	while (discard)
	{
	    /*
	     * The current line is longer than LINE_MAXLEN, continue
	     * reading but discard everything until EOL or EOF is
	     * reached.
	     */
	    if (fgets((char *)IObuff, IOSIZE, state->fd) == NULL
		    || (int)STRLEN(IObuff) < IOSIZE - 1
		    || IObuff[IOSIZE - 1] == '\n')
		break;
	}

	state->linebuf = state->growbuf;
	state->linelen = growbuflen;
    }
    else
	state->linebuf = IObuff;

#ifdef FEAT_MBYTE
    /* Convert a line if it contains a non-ASCII character. */
    if (state->vc.vc_type != CONV_NONE && has_non_ascii(state->linebuf))
    {
	char_u	*line;

	line = string_convert(&state->vc, state->linebuf, &state->linelen);
	if (line != NULL)
	{
	    if (state->linelen < IOSIZE)
	    {
		STRCPY(state->linebuf, line);
		vim_free(line);
	    }
	    else
	    {
		vim_free(state->growbuf);
		state->linebuf = state->growbuf = line;
		state->growbufsiz = state->linelen < LINE_MAXLEN
						? state->linelen : LINE_MAXLEN;
	    }
	}
    }
#endif

    return QF_OK;
}

/*
 * Get the next string from a file/buffer/list/string.
 */
    static int
qf_get_nextline(qfstate_T *state)
{
    int status = QF_FAIL;

    if (state->fd == NULL)
    {
	if (state->tv != NULL)
	{
	    if (state->tv->v_type == VAR_STRING)
		/* Get the next line from the supplied string */
		status = qf_get_next_str_line(state);
	    else if (state->tv->v_type == VAR_LIST)
		/* Get the next line from the supplied list */
		status = qf_get_next_list_line(state);
	}
	else
	    /* Get the next line from the supplied buffer */
	    status = qf_get_next_buf_line(state);
    }
    else
	/* Get the next line from the supplied file */
	status = qf_get_next_file_line(state);

    if (status != QF_OK)
	return status;

    /* remove newline/CR from the line */
    if (state->linelen > 0 && state->linebuf[state->linelen - 1] == '\n')
    {
	state->linebuf[state->linelen - 1] = NUL;
#ifdef USE_CRNL
	if (state->linelen > 1 && state->linebuf[state->linelen - 2] == '\r')
	    state->linebuf[state->linelen - 2] = NUL;
#endif
    }

#ifdef FEAT_MBYTE
    remove_bom(state->linebuf);
#endif

    return QF_OK;
}

typedef struct {
    char_u	*namebuf;
    char_u	*module;
    char_u	*errmsg;
    int		errmsglen;
    long	lnum;
    int		col;
    char_u	use_viscol;
    char_u	*pattern;
    int		enr;
    int		type;
    int		valid;
} qffields_T;

/*
 * Parse the error format matches in 'regmatch' and set the values in 'fields'.
 * fmt_ptr contains the 'efm' format specifiers/prefixes that have a match.
 * Returns QF_OK if all the matches are successfully parsed. On failure,
 * returns QF_FAIL or QF_NOMEM.
 */
    static int
qf_parse_match(
	char_u		*linebuf,
	int		linelen,
	efm_T		*fmt_ptr,
	regmatch_T	*regmatch,
	qffields_T	*fields,
	int		qf_multiline,
	int		qf_multiscan,
	char_u		**tail)
{
    char_u	*p;
    int		idx = fmt_ptr->prefix;
    int		i;
    int		len;

    if ((idx == 'C' || idx == 'Z') && !qf_multiline)
	return QF_FAIL;
    if (vim_strchr((char_u *)"EWI", idx) != NULL)
	fields->type = idx;
    else
	fields->type = 0;
    /*
     * Extract error message data from matched line.
     * We check for an actual submatch, because "\[" and "\]" in
     * the 'errorformat' may cause the wrong submatch to be used.
     */
    if ((i = (int)fmt_ptr->addr[0]) > 0)		/* %f */
    {
	int c;

	if (regmatch->startp[i] == NULL || regmatch->endp[i] == NULL)
	    return QF_FAIL;

	/* Expand ~/file and $HOME/file to full path. */
	c = *regmatch->endp[i];
	*regmatch->endp[i] = NUL;
	expand_env(regmatch->startp[i], fields->namebuf, CMDBUFFSIZE);
	*regmatch->endp[i] = c;

	if (vim_strchr((char_u *)"OPQ", idx) != NULL
		&& mch_getperm(fields->namebuf) == -1)
	    return QF_FAIL;
    }
    if ((i = (int)fmt_ptr->addr[1]) > 0)		/* %n */
    {
	if (regmatch->startp[i] == NULL)
	    return QF_FAIL;
	fields->enr = (int)atol((char *)regmatch->startp[i]);
    }
    if ((i = (int)fmt_ptr->addr[2]) > 0)		/* %l */
    {
	if (regmatch->startp[i] == NULL)
	    return QF_FAIL;
	fields->lnum = atol((char *)regmatch->startp[i]);
    }
    if ((i = (int)fmt_ptr->addr[3]) > 0)		/* %c */
    {
	if (regmatch->startp[i] == NULL)
	    return QF_FAIL;
	fields->col = (int)atol((char *)regmatch->startp[i]);
    }
    if ((i = (int)fmt_ptr->addr[4]) > 0)		/* %t */
    {
	if (regmatch->startp[i] == NULL)
	    return QF_FAIL;
	fields->type = *regmatch->startp[i];
    }
    if (fmt_ptr->flags == '+' && !qf_multiscan)	/* %+ */
    {
	if (linelen >= fields->errmsglen)
	{
	    /* linelen + null terminator */
	    if ((p = vim_realloc(fields->errmsg, linelen + 1)) == NULL)
		return QF_NOMEM;
	    fields->errmsg = p;
	    fields->errmsglen = linelen + 1;
	}
	vim_strncpy(fields->errmsg, linebuf, linelen);
    }
    else if ((i = (int)fmt_ptr->addr[5]) > 0)	/* %m */
    {
	if (regmatch->startp[i] == NULL || regmatch->endp[i] == NULL)
	    return QF_FAIL;
	len = (int)(regmatch->endp[i] - regmatch->startp[i]);
	if (len >= fields->errmsglen)
	{
	    /* len + null terminator */
	    if ((p = vim_realloc(fields->errmsg, len + 1)) == NULL)
		return QF_NOMEM;
	    fields->errmsg = p;
	    fields->errmsglen = len + 1;
	}
	vim_strncpy(fields->errmsg, regmatch->startp[i], len);
    }
    if ((i = (int)fmt_ptr->addr[6]) > 0)		/* %r */
    {
	if (regmatch->startp[i] == NULL)
	    return QF_FAIL;
	*tail = regmatch->startp[i];
    }
    if ((i = (int)fmt_ptr->addr[7]) > 0)		/* %p */
    {
	char_u	*match_ptr;

	if (regmatch->startp[i] == NULL || regmatch->endp[i] == NULL)
	    return QF_FAIL;
	fields->col = 0;
	for (match_ptr = regmatch->startp[i];
		match_ptr != regmatch->endp[i]; ++match_ptr)
	{
	    ++fields->col;
	    if (*match_ptr == TAB)
	    {
		fields->col += 7;
		fields->col -= fields->col % 8;
	    }
	}
	++fields->col;
	fields->use_viscol = TRUE;
    }
    if ((i = (int)fmt_ptr->addr[8]) > 0)		/* %v */
    {
	if (regmatch->startp[i] == NULL)
	    return QF_FAIL;
	fields->col = (int)atol((char *)regmatch->startp[i]);
	fields->use_viscol = TRUE;
    }
    if ((i = (int)fmt_ptr->addr[9]) > 0)		/* %s */
    {
	if (regmatch->startp[i] == NULL || regmatch->endp[i] == NULL)
	    return QF_FAIL;
	len = (int)(regmatch->endp[i] - regmatch->startp[i]);
	if (len > CMDBUFFSIZE - 5)
	    len = CMDBUFFSIZE - 5;
	STRCPY(fields->pattern, "^\\V");
	STRNCAT(fields->pattern, regmatch->startp[i], len);
	fields->pattern[len + 3] = '\\';
	fields->pattern[len + 4] = '$';
	fields->pattern[len + 5] = NUL;
    }
    if ((i = (int)fmt_ptr->addr[10]) > 0)		/* %o */
    {
	if (regmatch->startp[i] == NULL || regmatch->endp[i] == NULL)
	    return QF_FAIL;
	len = (int)(regmatch->endp[i] - regmatch->startp[i]);
	if (len > CMDBUFFSIZE)
	    len = CMDBUFFSIZE;
	STRNCAT(fields->module, regmatch->startp[i], len);
    }

    return QF_OK;
}

/*
 * Parse an error line in 'linebuf' using a single error format string in
 * 'fmt_ptr->prog' and return the matching values in 'fields'.
 * Returns QF_OK if the efm format matches completely and the fields are
 * successfully copied. Otherwise returns QF_FAIL or QF_NOMEM.
 */
    static int
qf_parse_get_fields(
	char_u		*linebuf,
	int		linelen,
	efm_T		*fmt_ptr,
	qffields_T	*fields,
	int		qf_multiline,
	int		qf_multiscan,
	char_u		**tail)
{
    regmatch_T	regmatch;
    int		status = QF_FAIL;
    int		r;

    if (qf_multiscan &&
		vim_strchr((char_u *)"OPQ", fmt_ptr->prefix) == NULL)
	return QF_FAIL;

    fields->namebuf[0] = NUL;
    fields->module[0] = NUL;
    fields->pattern[0] = NUL;
    if (!qf_multiscan)
	fields->errmsg[0] = NUL;
    fields->lnum = 0;
    fields->col = 0;
    fields->use_viscol = FALSE;
    fields->enr = -1;
    fields->type = 0;
    *tail = NULL;

    /* Always ignore case when looking for a matching error. */
    regmatch.rm_ic = TRUE;
    regmatch.regprog = fmt_ptr->prog;
    r = vim_regexec(&regmatch, linebuf, (colnr_T)0);
    fmt_ptr->prog = regmatch.regprog;
    if (r)
	status = qf_parse_match(linebuf, linelen, fmt_ptr, &regmatch,
		fields, qf_multiline, qf_multiscan, tail);

    return status;
}

/*
 * Parse directory error format prefixes (%D and %X).
 * Push and pop directories from the directory stack when scanning directory
 * names.
 */
    static int
qf_parse_dir_pfx(int idx, qffields_T *fields, qf_list_T *qfl)
{
    if (idx == 'D')				/* enter directory */
    {
	if (*fields->namebuf == NUL)
	{
	    EMSG(_("E379: Missing or empty directory name"));
	    return QF_FAIL;
	}
	qfl->qf_directory =
	    qf_push_dir(fields->namebuf, &qfl->qf_dir_stack, FALSE);
	if (qfl->qf_directory == NULL)
	    return QF_FAIL;
    }
    else if (idx == 'X')			/* leave directory */
	qfl->qf_directory = qf_pop_dir(&qfl->qf_dir_stack);

    return QF_OK;
}

/*
 * Parse global file name error format prefixes (%O, %P and %Q).
 */
    static int
qf_parse_file_pfx(
	int idx,
	qffields_T *fields,
	qf_list_T *qfl,
	char_u *tail)
{
    fields->valid = FALSE;
    if (*fields->namebuf == NUL || mch_getperm(fields->namebuf) >= 0)
    {
	if (*fields->namebuf && idx == 'P')
	    qfl->qf_currfile =
		qf_push_dir(fields->namebuf, &qfl->qf_file_stack, TRUE);
	else if (idx == 'Q')
	    qfl->qf_currfile = qf_pop_dir(&qfl->qf_file_stack);
	*fields->namebuf = NUL;
	if (tail && *tail)
	{
	    STRMOVE(IObuff, skipwhite(tail));
	    qfl->qf_multiscan = TRUE;
	    return QF_MULTISCAN;
	}
    }

    return QF_OK;
}

/*
 * Parse a non-error line (a line which doesn't match any of the error
 * format in 'efm').
 */
    static int
qf_parse_line_nomatch(char_u *linebuf, int linelen, qffields_T *fields)
{
    char_u	*p;

    fields->namebuf[0] = NUL;	/* no match found, remove file name */
    fields->lnum = 0;			/* don't jump to this line */
    fields->valid = FALSE;
    if (linelen >= fields->errmsglen)
    {
	/* linelen + null terminator */
	if ((p = vim_realloc(fields->errmsg, linelen + 1)) == NULL)
	    return QF_NOMEM;
	fields->errmsg = p;
	fields->errmsglen = linelen + 1;
    }
    /* copy whole line to error message */
    vim_strncpy(fields->errmsg, linebuf, linelen);

    return QF_OK;
}

/*
 * Parse multi-line error format prefixes (%C and %Z)
 */
    static int
qf_parse_multiline_pfx(
	qf_info_T *qi,
	int qf_idx,
	int idx,
	qf_list_T *qfl,
	qffields_T *fields)
{
    char_u		*ptr;
    int			len;

    if (!qfl->qf_multiignore)
    {
	qfline_T *qfprev = qfl->qf_last;

	if (qfprev == NULL)
	    return QF_FAIL;
	if (*fields->errmsg && !qfl->qf_multiignore)
	{
	    len = (int)STRLEN(qfprev->qf_text);
	    if ((ptr = alloc((unsigned)(len + STRLEN(fields->errmsg) + 2)))
		    == NULL)
		return QF_FAIL;
	    STRCPY(ptr, qfprev->qf_text);
	    vim_free(qfprev->qf_text);
	    qfprev->qf_text = ptr;
	    *(ptr += len) = '\n';
	    STRCPY(++ptr, fields->errmsg);
	}
	if (qfprev->qf_nr == -1)
	    qfprev->qf_nr = fields->enr;
	if (vim_isprintc(fields->type) && !qfprev->qf_type)
	    /* only printable chars allowed */
	    qfprev->qf_type = fields->type;

	if (!qfprev->qf_lnum)
	    qfprev->qf_lnum = fields->lnum;
	if (!qfprev->qf_col)
	    qfprev->qf_col = fields->col;
	qfprev->qf_viscol = fields->use_viscol;
	if (!qfprev->qf_fnum)
	    qfprev->qf_fnum = qf_get_fnum(qi, qf_idx,
		    qfl->qf_directory,
		    *fields->namebuf || qfl->qf_directory != NULL
		    ? fields->namebuf
		    : qfl->qf_currfile != NULL && fields->valid
		    ? qfl->qf_currfile : 0);
    }
    if (idx == 'Z')
	qfl->qf_multiline = qfl->qf_multiignore = FALSE;
    line_breakcheck();

    return QF_IGNORE_LINE;
}

/*
 * Parse a line and get the quickfix fields.
 * Return the QF_ status.
 */
    static int
qf_parse_line(
	qf_info_T	*qi,
	int		qf_idx,
	char_u		*linebuf,
	int		linelen,
	efm_T		*fmt_first,
	qffields_T	*fields)
{
    efm_T		*fmt_ptr;
    int			idx = 0;
    char_u		*tail = NULL;
    qf_list_T		*qfl = &qi->qf_lists[qf_idx];
    int			status;

restofline:
    /* If there was no %> item start at the first pattern */
    if (fmt_start == NULL)
	fmt_ptr = fmt_first;
    else
    {
	/* Otherwise start from the last used pattern */
	fmt_ptr = fmt_start;
	fmt_start = NULL;
    }

    /*
     * Try to match each part of 'errorformat' until we find a complete
     * match or no match.
     */
    fields->valid = TRUE;
    for ( ; fmt_ptr != NULL; fmt_ptr = fmt_ptr->next)
    {
	idx = fmt_ptr->prefix;
	status = qf_parse_get_fields(linebuf, linelen, fmt_ptr, fields,
				qfl->qf_multiline, qfl->qf_multiscan, &tail);
	if (status == QF_NOMEM)
	    return status;
	if (status == QF_OK)
	    break;
    }
    qfl->qf_multiscan = FALSE;

    if (fmt_ptr == NULL || idx == 'D' || idx == 'X')
    {
	if (fmt_ptr != NULL)
	{
	    /* 'D' and 'X' directory specifiers */
	    status = qf_parse_dir_pfx(idx, fields, qfl);
	    if (status != QF_OK)
		return status;
	}

	status = qf_parse_line_nomatch(linebuf, linelen, fields);
	if (status != QF_OK)
	    return status;

	if (fmt_ptr == NULL)
	    qfl->qf_multiline = qfl->qf_multiignore = FALSE;
    }
    else if (fmt_ptr != NULL)
    {
	/* honor %> item */
	if (fmt_ptr->conthere)
	    fmt_start = fmt_ptr;

	if (vim_strchr((char_u *)"AEWI", idx) != NULL)
	{
	    qfl->qf_multiline = TRUE;	/* start of a multi-line message */
	    qfl->qf_multiignore = FALSE;/* reset continuation */
	}
	else if (vim_strchr((char_u *)"CZ", idx) != NULL)
	{				/* continuation of multi-line msg */
	    status = qf_parse_multiline_pfx(qi, qf_idx, idx, qfl, fields);
	    if (status != QF_OK)
		return status;
	}
	else if (vim_strchr((char_u *)"OPQ", idx) != NULL)
	{				/* global file names */
	    status = qf_parse_file_pfx(idx, fields, qfl, tail);
	    if (status == QF_MULTISCAN)
		goto restofline;
	}
	if (fmt_ptr->flags == '-')	/* generally exclude this line */
	{
	    if (qfl->qf_multiline)
		/* also exclude continuation lines */
		qfl->qf_multiignore = TRUE;
	    return QF_IGNORE_LINE;
	}
    }

    return QF_OK;
}

/*
 * Read the errorfile "efile" into memory, line by line, building the error
 * list.
 * Alternative: when "efile" is NULL read errors from buffer "buf".
 * Alternative: when "tv" is not NULL get errors from the string or list.
 * Always use 'errorformat' from "buf" if there is a local value.
 * Then "lnumfirst" and "lnumlast" specify the range of lines to use.
 * Set the title of the list to "qf_title".
 * Return -1 for error, number of errors for success.
 */
    static int
qf_init_ext(
    qf_info_T	    *qi,
    int		    qf_idx,
    char_u	    *efile,
    buf_T	    *buf,
    typval_T	    *tv,
    char_u	    *errorformat,
    int		    newlist,		/* TRUE: start a new error list */
    linenr_T	    lnumfirst,		/* first line number to use */
    linenr_T	    lnumlast,		/* last line number to use */
    char_u	    *qf_title,
    char_u	    *enc)
{
    qf_list_T	    *qfl;
    qfstate_T	    state;
    qffields_T	    fields;
    qfline_T	    *old_last = NULL;
    int		    adding = FALSE;
    static efm_T    *fmt_first = NULL;
    char_u	    *efm;
    static char_u   *last_efm = NULL;
    int		    retval = -1;	/* default: return error flag */
    int		    status;

    /* Do not used the cached buffer, it may have been wiped out. */
    VIM_CLEAR(qf_last_bufname);

    vim_memset(&state, 0, sizeof(state));
    vim_memset(&fields, 0, sizeof(fields));
#ifdef FEAT_MBYTE
    state.vc.vc_type = CONV_NONE;
    if (enc != NULL && *enc != NUL)
	convert_setup(&state.vc, enc, p_enc);
#endif
    fields.namebuf = alloc_id(CMDBUFFSIZE + 1, aid_qf_namebuf);
    fields.module = alloc_id(CMDBUFFSIZE + 1, aid_qf_module);
    fields.errmsglen = CMDBUFFSIZE + 1;
    fields.errmsg = alloc_id(fields.errmsglen, aid_qf_errmsg);
    fields.pattern = alloc_id(CMDBUFFSIZE + 1, aid_qf_pattern);
    if (fields.namebuf == NULL || fields.errmsg == NULL
		|| fields.pattern == NULL || fields.module == NULL)
	goto qf_init_end;

    if (efile != NULL && (state.fd = mch_fopen((char *)efile, "r")) == NULL)
    {
	EMSG2(_(e_openerrf), efile);
	goto qf_init_end;
    }

    if (newlist || qf_idx == qi->qf_listcount)
    {
	/* make place for a new list */
	qf_new_list(qi, qf_title);
	qf_idx = qi->qf_curlist;
    }
    else
    {
	/* Adding to existing list, use last entry. */
	adding = TRUE;
	if (qi->qf_lists[qf_idx].qf_count > 0)
	    old_last = qi->qf_lists[qf_idx].qf_last;
    }

    qfl = &qi->qf_lists[qf_idx];

    /* Use the local value of 'errorformat' if it's set. */
    if (errorformat == p_efm && tv == NULL && *buf->b_p_efm != NUL)
	efm = buf->b_p_efm;
    else
	efm = errorformat;

    /*
     * If the errorformat didn't change between calls, then reuse the
     * previously parsed values.
     */
    if (last_efm == NULL || (STRCMP(last_efm, efm) != 0))
    {
	/* free the previously parsed data */
	VIM_CLEAR(last_efm);
	free_efm_list(&fmt_first);

	/* parse the current 'efm' */
	fmt_first = parse_efm_option(efm);
	if (fmt_first != NULL)
	    last_efm = vim_strsave(efm);
    }

    if (fmt_first == NULL)	/* nothing found */
	goto error2;

    /*
     * got_int is reset here, because it was probably set when killing the
     * ":make" command, but we still want to read the errorfile then.
     */
    got_int = FALSE;

    if (tv != NULL)
    {
	if (tv->v_type == VAR_STRING)
	    state.p_str = tv->vval.v_string;
	else if (tv->v_type == VAR_LIST)
	    state.p_li = tv->vval.v_list->lv_first;
	state.tv = tv;
    }
    state.buf = buf;
    state.buflnum = lnumfirst;
    state.lnumlast = lnumlast;

    /*
     * Read the lines in the error file one by one.
     * Try to recognize one of the error formats in each line.
     */
    while (!got_int)
    {
	/* Get the next line from a file/buffer/list/string */
	status = qf_get_nextline(&state);
	if (status == QF_NOMEM)		/* memory alloc failure */
	    goto qf_init_end;
	if (status == QF_END_OF_INPUT)	/* end of input */
	    break;

	status = qf_parse_line(qi, qf_idx, state.linebuf, state.linelen,
							fmt_first, &fields);
	if (status == QF_FAIL)
	    goto error2;
	if (status == QF_NOMEM)
	    goto qf_init_end;
	if (status == QF_IGNORE_LINE)
	    continue;

	if (qf_add_entry(qi,
			qf_idx,
			qfl->qf_directory,
			(*fields.namebuf || qfl->qf_directory != NULL)
			    ? fields.namebuf
			    : ((qfl->qf_currfile != NULL && fields.valid)
				? qfl->qf_currfile : (char_u *)NULL),
			fields.module,
			0,
			fields.errmsg,
			fields.lnum,
			fields.col,
			fields.use_viscol,
			fields.pattern,
			fields.enr,
			fields.type,
			fields.valid) == FAIL)
	    goto error2;
	line_breakcheck();
    }
    if (state.fd == NULL || !ferror(state.fd))
    {
	if (qfl->qf_index == 0)
	{
	    /* no valid entry found */
	    qfl->qf_ptr = qfl->qf_start;
	    qfl->qf_index = 1;
	    qfl->qf_nonevalid = TRUE;
	}
	else
	{
	    qfl->qf_nonevalid = FALSE;
	    if (qfl->qf_ptr == NULL)
		qfl->qf_ptr = qfl->qf_start;
	}
	/* return number of matches */
	retval = qfl->qf_count;
	goto qf_init_end;
    }
    EMSG(_(e_readerrf));
error2:
    if (!adding)
    {
	/* Error when creating a new list. Free the new list */
	qf_free(qi, qi->qf_curlist);
	qi->qf_listcount--;
	if (qi->qf_curlist > 0)
	    --qi->qf_curlist;
    }
qf_init_end:
    if (state.fd != NULL)
	fclose(state.fd);
    vim_free(fields.namebuf);
    vim_free(fields.module);
    vim_free(fields.errmsg);
    vim_free(fields.pattern);
    vim_free(state.growbuf);

    if (qf_idx == qi->qf_curlist)
	qf_update_buffer(qi, old_last);
#ifdef FEAT_MBYTE
    if (state.vc.vc_type != CONV_NONE)
	convert_setup(&state.vc, NULL, NULL);
#endif

    return retval;
}

/*
 * Set the title of the specified quickfix list. Frees the previous title.
 * Prepends ':' to the title.
 */
    static void
qf_store_title(qf_info_T *qi, int qf_idx, char_u *title)
{
    VIM_CLEAR(qi->qf_lists[qf_idx].qf_title);

    if (title != NULL)
    {
	char_u *p = alloc((int)STRLEN(title) + 2);

	qi->qf_lists[qf_idx].qf_title = p;
	if (p != NULL)
	    STRCPY(p, title);
    }
}

/*
 * The title of a quickfix/location list is set, by default, to the command
 * that created the quickfix list with the ":" prefix.
 * Create a quickfix list title string by prepending ":" to a user command.
 * Returns a pointer to a static buffer with the title.
 */
    static char_u *
qf_cmdtitle(char_u *cmd)
{
    static char_u qftitle_str[IOSIZE];

    vim_snprintf((char *)qftitle_str, IOSIZE, ":%s", (char *)cmd);
    return qftitle_str;
}

/*
 * Prepare for adding a new quickfix list. If the current list is in the
 * middle of the stack, then all the following lists are freed and then
 * the new list is added.
 */
    static void
qf_new_list(qf_info_T *qi, char_u *qf_title)
{
    int		i;

    /*
     * If the current entry is not the last entry, delete entries beyond
     * the current entry.  This makes it possible to browse in a tree-like
     * way with ":grep'.
     */
    while (qi->qf_listcount > qi->qf_curlist + 1)
	qf_free(qi, --qi->qf_listcount);

    /*
     * When the stack is full, remove to oldest entry
     * Otherwise, add a new entry.
     */
    if (qi->qf_listcount == LISTCOUNT)
    {
	qf_free(qi, 0);
	for (i = 1; i < LISTCOUNT; ++i)
	    qi->qf_lists[i - 1] = qi->qf_lists[i];
	qi->qf_curlist = LISTCOUNT - 1;
    }
    else
	qi->qf_curlist = qi->qf_listcount++;
    vim_memset(&qi->qf_lists[qi->qf_curlist], 0, (size_t)(sizeof(qf_list_T)));
    qf_store_title(qi, qi->qf_curlist, qf_title);
    qi->qf_lists[qi->qf_curlist].qf_id = ++last_qf_id;
}

/*
 * Free a location list
 */
    static void
ll_free_all(qf_info_T **pqi)
{
    int		i;
    qf_info_T	*qi;

    qi = *pqi;
    if (qi == NULL)
	return;
    *pqi = NULL;	/* Remove reference to this list */

    qi->qf_refcount--;
    if (qi->qf_refcount < 1)
    {
	/* No references to this location list */
	for (i = 0; i < qi->qf_listcount; ++i)
	    qf_free(qi, i);
	vim_free(qi);
    }
}

/*
 * Free all the quickfix/location lists in the stack.
 */
    void
qf_free_all(win_T *wp)
{
    int		i;
    qf_info_T	*qi = &ql_info;

    if (wp != NULL)
    {
	/* location list */
	ll_free_all(&wp->w_llist);
	ll_free_all(&wp->w_llist_ref);
    }
    else
	/* quickfix list */
	for (i = 0; i < qi->qf_listcount; ++i)
	    qf_free(qi, i);
}

/*
 * Add an entry to the end of the list of errors.
 * Returns OK or FAIL.
 */
    static int
qf_add_entry(
    qf_info_T	*qi,		/* quickfix list */
    int		qf_idx,		/* list index */
    char_u	*dir,		/* optional directory name */
    char_u	*fname,		/* file name or NULL */
    char_u	*module,	/* module name or NULL */
    int		bufnum,		/* buffer number or zero */
    char_u	*mesg,		/* message */
    long	lnum,		/* line number */
    int		col,		/* column */
    int		vis_col,	/* using visual column */
    char_u	*pattern,	/* search pattern */
    int		nr,		/* error number */
    int		type,		/* type character */
    int		valid)		/* valid entry */
{
    qfline_T	*qfp;
    qfline_T	**lastp;	/* pointer to qf_last or NULL */

    if ((qfp = (qfline_T *)alloc((unsigned)sizeof(qfline_T))) == NULL)
	return FAIL;
    if (bufnum != 0)
    {
	buf_T *buf = buflist_findnr(bufnum);

	qfp->qf_fnum = bufnum;
	if (buf != NULL)
	    buf->b_has_qf_entry |=
		(qi == &ql_info) ? BUF_HAS_QF_ENTRY : BUF_HAS_LL_ENTRY;
    }
    else
	qfp->qf_fnum = qf_get_fnum(qi, qf_idx, dir, fname);
    if ((qfp->qf_text = vim_strsave(mesg)) == NULL)
    {
	vim_free(qfp);
	return FAIL;
    }
    qfp->qf_lnum = lnum;
    qfp->qf_col = col;
    qfp->qf_viscol = vis_col;
    if (pattern == NULL || *pattern == NUL)
	qfp->qf_pattern = NULL;
    else if ((qfp->qf_pattern = vim_strsave(pattern)) == NULL)
    {
	vim_free(qfp->qf_text);
	vim_free(qfp);
	return FAIL;
    }
    if (module == NULL || *module == NUL)
	qfp->qf_module = NULL;
    else if ((qfp->qf_module = vim_strsave(module)) == NULL)
    {
	vim_free(qfp->qf_text);
	vim_free(qfp->qf_pattern);
	vim_free(qfp);
	return FAIL;
    }
    qfp->qf_nr = nr;
    if (type != 1 && !vim_isprintc(type)) /* only printable chars allowed */
	type = 0;
    qfp->qf_type = type;
    qfp->qf_valid = valid;

    lastp = &qi->qf_lists[qf_idx].qf_last;
    if (qi->qf_lists[qf_idx].qf_count == 0)
				/* first element in the list */
    {
	qi->qf_lists[qf_idx].qf_start = qfp;
	qi->qf_lists[qf_idx].qf_ptr = qfp;
	qi->qf_lists[qf_idx].qf_index = 0;
	qfp->qf_prev = NULL;
    }
    else
    {
	qfp->qf_prev = *lastp;
	(*lastp)->qf_next = qfp;
    }
    qfp->qf_next = NULL;
    qfp->qf_cleared = FALSE;
    *lastp = qfp;
    ++qi->qf_lists[qf_idx].qf_count;
    if (qi->qf_lists[qf_idx].qf_index == 0 && qfp->qf_valid)
				/* first valid entry */
    {
	qi->qf_lists[qf_idx].qf_index =
	    qi->qf_lists[qf_idx].qf_count;
	qi->qf_lists[qf_idx].qf_ptr = qfp;
    }

    return OK;
}

/*
 * Allocate a new location list
 */
    static qf_info_T *
ll_new_list(void)
{
    qf_info_T *qi;

    qi = (qf_info_T *)alloc((unsigned)sizeof(qf_info_T));
    if (qi != NULL)
    {
	vim_memset(qi, 0, (size_t)(sizeof(qf_info_T)));
	qi->qf_refcount++;
    }

    return qi;
}

/*
 * Return the location list for window 'wp'.
 * If not present, allocate a location list
 */
    static qf_info_T *
ll_get_or_alloc_list(win_T *wp)
{
    if (IS_LL_WINDOW(wp))
	/* For a location list window, use the referenced location list */
	return wp->w_llist_ref;

    /*
     * For a non-location list window, w_llist_ref should not point to a
     * location list.
     */
    ll_free_all(&wp->w_llist_ref);

    if (wp->w_llist == NULL)
	wp->w_llist = ll_new_list();	    /* new location list */
    return wp->w_llist;
}

/*
 * Copy the location list from window "from" to window "to".
 */
    void
copy_loclist(win_T *from, win_T *to)
{
    qf_info_T	*qi;
    int		idx;
    int		i;

    /*
     * When copying from a location list window, copy the referenced
     * location list. For other windows, copy the location list for
     * that window.
     */
    if (IS_LL_WINDOW(from))
	qi = from->w_llist_ref;
    else
	qi = from->w_llist;

    if (qi == NULL)		    /* no location list to copy */
	return;

    /* allocate a new location list */
    if ((to->w_llist = ll_new_list()) == NULL)
	return;

    to->w_llist->qf_listcount = qi->qf_listcount;

    /* Copy the location lists one at a time */
    for (idx = 0; idx < qi->qf_listcount; idx++)
    {
	qf_list_T   *from_qfl;
	qf_list_T   *to_qfl;

	to->w_llist->qf_curlist = idx;

	from_qfl = &qi->qf_lists[idx];
	to_qfl = &to->w_llist->qf_lists[idx];

	/* Some of the fields are populated by qf_add_entry() */
	to_qfl->qf_nonevalid = from_qfl->qf_nonevalid;
	to_qfl->qf_count = 0;
	to_qfl->qf_index = 0;
	to_qfl->qf_start = NULL;
	to_qfl->qf_last = NULL;
	to_qfl->qf_ptr = NULL;
	if (from_qfl->qf_title != NULL)
	    to_qfl->qf_title = vim_strsave(from_qfl->qf_title);
	else
	    to_qfl->qf_title = NULL;
	if (from_qfl->qf_ctx != NULL)
	{
	    to_qfl->qf_ctx = alloc_tv();
	    if (to_qfl->qf_ctx != NULL)
		copy_tv(from_qfl->qf_ctx, to_qfl->qf_ctx);
	}
	else
	    to_qfl->qf_ctx = NULL;

	if (from_qfl->qf_count)
	{
	    qfline_T    *from_qfp;
	    qfline_T    *prevp;

	    /* copy all the location entries in this list */
	    for (i = 0, from_qfp = from_qfl->qf_start;
		    i < from_qfl->qf_count && from_qfp != NULL;
		    ++i, from_qfp = from_qfp->qf_next)
	    {
		if (qf_add_entry(to->w_llist,
				 to->w_llist->qf_curlist,
				 NULL,
				 NULL,
				 from_qfp->qf_module,
				 0,
				 from_qfp->qf_text,
				 from_qfp->qf_lnum,
				 from_qfp->qf_col,
				 from_qfp->qf_viscol,
				 from_qfp->qf_pattern,
				 from_qfp->qf_nr,
				 0,
				 from_qfp->qf_valid) == FAIL)
		{
		    qf_free_all(to);
		    return;
		}
		/*
		 * qf_add_entry() will not set the qf_num field, as the
		 * directory and file names are not supplied. So the qf_fnum
		 * field is copied here.
		 */
		prevp = to->w_llist->qf_lists[to->w_llist->qf_curlist].qf_last;
		prevp->qf_fnum = from_qfp->qf_fnum; /* file number */
		prevp->qf_type = from_qfp->qf_type; /* error type */
		if (from_qfl->qf_ptr == from_qfp)
		    to_qfl->qf_ptr = prevp;	    /* current location */
	    }
	}

	to_qfl->qf_index = from_qfl->qf_index;	/* current index in the list */

	/* Assign a new ID for the location list */
	to_qfl->qf_id = ++last_qf_id;
	to_qfl->qf_changedtick = 0L;

	/* When no valid entries are present in the list, qf_ptr points to
	 * the first item in the list */
	if (to_qfl->qf_nonevalid)
	{
	    to_qfl->qf_ptr = to_qfl->qf_start;
	    to_qfl->qf_index = 1;
	}
    }

    to->w_llist->qf_curlist = qi->qf_curlist;	/* current list */
}

/*
 * Get buffer number for file "directory/fname".
 * Also sets the b_has_qf_entry flag.
 */
    static int
qf_get_fnum(qf_info_T *qi, int qf_idx, char_u *directory, char_u *fname)
{
    char_u	*ptr = NULL;
    buf_T	*buf;
    char_u	*bufname;

    if (fname == NULL || *fname == NUL)		/* no file name */
	return 0;

#ifdef VMS
    vms_remove_version(fname);
#endif
#ifdef BACKSLASH_IN_FILENAME
    if (directory != NULL)
	slash_adjust(directory);
    slash_adjust(fname);
#endif
    if (directory != NULL && !vim_isAbsName(fname)
	    && (ptr = concat_fnames(directory, fname, TRUE)) != NULL)
    {
	/*
	 * Here we check if the file really exists.
	 * This should normally be true, but if make works without
	 * "leaving directory"-messages we might have missed a
	 * directory change.
	 */
	if (mch_getperm(ptr) < 0)
	{
	    vim_free(ptr);
	    directory = qf_guess_filepath(qi, qf_idx, fname);
	    if (directory)
		ptr = concat_fnames(directory, fname, TRUE);
	    else
		ptr = vim_strsave(fname);
	}
	/* Use concatenated directory name and file name */
	bufname = ptr;
    }
    else
	bufname = fname;

    if (qf_last_bufname != NULL && STRCMP(bufname, qf_last_bufname) == 0
	    && bufref_valid(&qf_last_bufref))
    {
	buf = qf_last_bufref.br_buf;
	vim_free(ptr);
    }
    else
    {
	vim_free(qf_last_bufname);
	buf = buflist_new(bufname, NULL, (linenr_T)0, BLN_NOOPT);
	if (bufname == ptr)
	    qf_last_bufname = bufname;
	else
	    qf_last_bufname = vim_strsave(bufname);
	set_bufref(&qf_last_bufref, buf);
    }
    if (buf == NULL)
	return 0;

    buf->b_has_qf_entry =
			(qi == &ql_info) ? BUF_HAS_QF_ENTRY : BUF_HAS_LL_ENTRY;
    return buf->b_fnum;
}

/*
 * Push dirbuf onto the directory stack and return pointer to actual dir or
 * NULL on error.
 */
    static char_u *
qf_push_dir(char_u *dirbuf, struct dir_stack_T **stackptr, int is_file_stack)
{
    struct dir_stack_T  *ds_new;
    struct dir_stack_T  *ds_ptr;

    /* allocate new stack element and hook it in */
    ds_new = (struct dir_stack_T *)alloc((unsigned)sizeof(struct dir_stack_T));
    if (ds_new == NULL)
	return NULL;

    ds_new->next = *stackptr;
    *stackptr = ds_new;

    /* store directory on the stack */
    if (vim_isAbsName(dirbuf)
	    || (*stackptr)->next == NULL
	    || (*stackptr && is_file_stack))
	(*stackptr)->dirname = vim_strsave(dirbuf);
    else
    {
	/* Okay we don't have an absolute path.
	 * dirbuf must be a subdir of one of the directories on the stack.
	 * Let's search...
	 */
	ds_new = (*stackptr)->next;
	(*stackptr)->dirname = NULL;
	while (ds_new)
	{
	    vim_free((*stackptr)->dirname);
	    (*stackptr)->dirname = concat_fnames(ds_new->dirname, dirbuf,
		    TRUE);
	    if (mch_isdir((*stackptr)->dirname) == TRUE)
		break;

	    ds_new = ds_new->next;
	}

	/* clean up all dirs we already left */
	while ((*stackptr)->next != ds_new)
	{
	    ds_ptr = (*stackptr)->next;
	    (*stackptr)->next = (*stackptr)->next->next;
	    vim_free(ds_ptr->dirname);
	    vim_free(ds_ptr);
	}

	/* Nothing found -> it must be on top level */
	if (ds_new == NULL)
	{
	    vim_free((*stackptr)->dirname);
	    (*stackptr)->dirname = vim_strsave(dirbuf);
	}
    }

    if ((*stackptr)->dirname != NULL)
	return (*stackptr)->dirname;
    else
    {
	ds_ptr = *stackptr;
	*stackptr = (*stackptr)->next;
	vim_free(ds_ptr);
	return NULL;
    }
}

/*
 * pop dirbuf from the directory stack and return previous directory or NULL if
 * stack is empty
 */
    static char_u *
qf_pop_dir(struct dir_stack_T **stackptr)
{
    struct dir_stack_T  *ds_ptr;

    /* TODO: Should we check if dirbuf is the directory on top of the stack?
     * What to do if it isn't? */

    /* pop top element and free it */
    if (*stackptr != NULL)
    {
	ds_ptr = *stackptr;
	*stackptr = (*stackptr)->next;
	vim_free(ds_ptr->dirname);
	vim_free(ds_ptr);
    }

    /* return NEW top element as current dir or NULL if stack is empty*/
    return *stackptr ? (*stackptr)->dirname : NULL;
}

/*
 * clean up directory stack
 */
    static void
qf_clean_dir_stack(struct dir_stack_T **stackptr)
{
    struct dir_stack_T  *ds_ptr;

    while ((ds_ptr = *stackptr) != NULL)
    {
	*stackptr = (*stackptr)->next;
	vim_free(ds_ptr->dirname);
	vim_free(ds_ptr);
    }
}

/*
 * Check in which directory of the directory stack the given file can be
 * found.
 * Returns a pointer to the directory name or NULL if not found.
 * Cleans up intermediate directory entries.
 *
 * TODO: How to solve the following problem?
 * If we have this directory tree:
 *     ./
 *     ./aa
 *     ./aa/bb
 *     ./bb
 *     ./bb/x.c
 * and make says:
 *     making all in aa
 *     making all in bb
 *     x.c:9: Error
 * Then qf_push_dir thinks we are in ./aa/bb, but we are in ./bb.
 * qf_guess_filepath will return NULL.
 */
    static char_u *
qf_guess_filepath(qf_info_T *qi, int qf_idx, char_u *filename)
{
    struct dir_stack_T     *ds_ptr;
    struct dir_stack_T     *ds_tmp;
    char_u		   *fullname;
    qf_list_T		   *qfl = &qi->qf_lists[qf_idx];

    /* no dirs on the stack - there's nothing we can do */
    if (qfl->qf_dir_stack == NULL)
	return NULL;

    ds_ptr = qfl->qf_dir_stack->next;
    fullname = NULL;
    while (ds_ptr)
    {
	vim_free(fullname);
	fullname = concat_fnames(ds_ptr->dirname, filename, TRUE);

	/* If concat_fnames failed, just go on. The worst thing that can happen
	 * is that we delete the entire stack.
	 */
	if ((fullname != NULL) && (mch_getperm(fullname) >= 0))
	    break;

	ds_ptr = ds_ptr->next;
    }

    vim_free(fullname);

    /* clean up all dirs we already left */
    while (qfl->qf_dir_stack->next != ds_ptr)
    {
	ds_tmp = qfl->qf_dir_stack->next;
	qfl->qf_dir_stack->next = qfl->qf_dir_stack->next->next;
	vim_free(ds_tmp->dirname);
	vim_free(ds_tmp);
    }

    return ds_ptr == NULL ? NULL : ds_ptr->dirname;
}

/*
 * Returns TRUE if a quickfix/location list with the given identifier exists.
 */
    static int
qflist_valid (win_T *wp, int_u qf_id)
{
    qf_info_T	*qi = &ql_info;
    int		i;

    if (wp != NULL)
    {
	qi = GET_LOC_LIST(wp);	    /* Location list */
	if (qi == NULL)
	    return FALSE;
    }

    for (i = 0; i < qi->qf_listcount; ++i)
	if (qi->qf_lists[i].qf_id == qf_id)
	    return TRUE;

    return FALSE;
}

/*
 * When loading a file from the quickfix, the auto commands may modify it.
 * This may invalidate the current quickfix entry.  This function checks
 * whether an entry is still present in the quickfix list.
 * Similar to location list.
 */
    static int
is_qf_entry_present(qf_info_T *qi, qfline_T *qf_ptr)
{
    qf_list_T	*qfl;
    qfline_T	*qfp;
    int		i;

    qfl = &qi->qf_lists[qi->qf_curlist];

    /* Search for the entry in the current list */
    for (i = 0, qfp = qfl->qf_start; i < qfl->qf_count;
	    ++i, qfp = qfp->qf_next)
	if (qfp == NULL || qfp == qf_ptr)
	    break;

    if (i == qfl->qf_count) /* Entry is not found */
	return FALSE;

    return TRUE;
}

/*
 * Get the next valid entry in the current quickfix/location list. The search
 * starts from the current entry.  Returns NULL on failure.
 */
    static qfline_T *
get_next_valid_entry(
	qf_info_T	*qi,
	qfline_T	*qf_ptr,
	int		*qf_index,
	int		dir)
{
    int			idx;
    int			old_qf_fnum;

    idx = *qf_index;
    old_qf_fnum = qf_ptr->qf_fnum;

    do
    {
	if (idx == qi->qf_lists[qi->qf_curlist].qf_count
		|| qf_ptr->qf_next == NULL)
	    return NULL;
	++idx;
	qf_ptr = qf_ptr->qf_next;
    } while ((!qi->qf_lists[qi->qf_curlist].qf_nonevalid
		&& !qf_ptr->qf_valid)
	    || (dir == FORWARD_FILE && qf_ptr->qf_fnum == old_qf_fnum));

    *qf_index = idx;
    return qf_ptr;
}

/*
 * Get the previous valid entry in the current quickfix/location list. The
 * search starts from the current entry.  Returns NULL on failure.
 */
    static qfline_T *
get_prev_valid_entry(
	qf_info_T	*qi,
	qfline_T	*qf_ptr,
	int		*qf_index,
	int		dir)
{
    int			idx;
    int			old_qf_fnum;

    idx = *qf_index;
    old_qf_fnum = qf_ptr->qf_fnum;

    do
    {
	if (idx == 1 || qf_ptr->qf_prev == NULL)
	    return NULL;
	--idx;
	qf_ptr = qf_ptr->qf_prev;
    } while ((!qi->qf_lists[qi->qf_curlist].qf_nonevalid
		&& !qf_ptr->qf_valid)
	    || (dir == BACKWARD_FILE && qf_ptr->qf_fnum == old_qf_fnum));

    *qf_index = idx;
    return qf_ptr;
}

/*
 * Get the n'th (errornr) previous/next valid entry from the current entry in
 * the quickfix list.
 *   dir == FORWARD or FORWARD_FILE: next valid entry
 *   dir == BACKWARD or BACKWARD_FILE: previous valid entry
 */
    static qfline_T *
get_nth_valid_entry(
	qf_info_T	*qi,
	int		errornr,
	qfline_T	*qf_ptr,
	int		*qf_index,
	int		dir)
{
    qfline_T		*prev_qf_ptr;
    int			prev_index;
    static char_u	*e_no_more_items = (char_u *)N_("E553: No more items");
    char_u		*err = e_no_more_items;

    while (errornr--)
    {
	prev_qf_ptr = qf_ptr;
	prev_index = *qf_index;

	if (dir == FORWARD || dir == FORWARD_FILE)
	    qf_ptr = get_next_valid_entry(qi, qf_ptr, qf_index, dir);
	else
	    qf_ptr = get_prev_valid_entry(qi, qf_ptr, qf_index, dir);
	if (qf_ptr == NULL)
	{
	    qf_ptr = prev_qf_ptr;
	    *qf_index = prev_index;
	    if (err != NULL)
	    {
		EMSG(_(err));
		return NULL;
	    }
	    break;
	}

	err = NULL;
    }

    return qf_ptr;
}

/*
 * Get n'th (errornr) quickfix entry
 */
    static qfline_T *
get_nth_entry(
	qf_info_T	*qi,
	int		errornr,
	qfline_T	*qf_ptr,
	int		*cur_qfidx)
{
    int		qf_idx = *cur_qfidx;

    /* New error number is less than the current error number */
    while (errornr < qf_idx && qf_idx > 1 && qf_ptr->qf_prev != NULL)
    {
	--qf_idx;
	qf_ptr = qf_ptr->qf_prev;
    }
    /* New error number is greater than the current error number */
    while (errornr > qf_idx &&
	    qf_idx < qi->qf_lists[qi->qf_curlist].qf_count &&
	    qf_ptr->qf_next != NULL)
    {
	++qf_idx;
	qf_ptr = qf_ptr->qf_next;
    }

    *cur_qfidx = qf_idx;
    return qf_ptr;
}

/*
 * Find a window displaying a Vim help file.
 */
    static win_T *
qf_find_help_win(void)
{
    win_T *wp;

    FOR_ALL_WINDOWS(wp)
	if (bt_help(wp->w_buffer))
	    return wp;

    return NULL;
}

/*
 * Find a help window or open one.
 */
    static int
jump_to_help_window(qf_info_T *qi, int *opened_window)
{
    win_T	*wp;
    int		flags;

    if (cmdmod.tab != 0)
	wp = NULL;
    else
	wp = qf_find_help_win();
    if (wp != NULL && wp->w_buffer->b_nwindows > 0)
	win_enter(wp, TRUE);
    else
    {
	/*
	 * Split off help window; put it at far top if no position
	 * specified, the current window is vertically split and narrow.
	 */
	flags = WSP_HELP;
	if (cmdmod.split == 0 && curwin->w_width != Columns
		&& curwin->w_width < 80)
	    flags |= WSP_TOP;
	if (qi != &ql_info)
	    flags |= WSP_NEWLOC;  /* don't copy the location list */

	if (win_split(0, flags) == FAIL)
	    return FAIL;

	*opened_window = TRUE;

	if (curwin->w_height < p_hh)
	    win_setheight((int)p_hh);

	if (qi != &ql_info)	    /* not a quickfix list */
	{
	    /* The new window should use the supplied location list */
	    curwin->w_llist = qi;
	    qi->qf_refcount++;
	}
    }

    if (!p_im)
	restart_edit = 0;	    /* don't want insert mode in help file */

    return OK;
}

/*
 * Find a non-quickfix window using the given location list.
 * Returns NULL if a matching window is not found.
 */
    static win_T *
qf_find_win_with_loclist(qf_info_T *ll)
{
    win_T	*wp;

    FOR_ALL_WINDOWS(wp)
	if (wp->w_llist == ll && !bt_quickfix(wp->w_buffer))
	    return wp;

    return NULL;
}

/*
 * Find a window containing a normal buffer
 */
    static win_T *
qf_find_win_with_normal_buf(void)
{
    win_T	*wp;

    FOR_ALL_WINDOWS(wp)
	if (wp->w_buffer->b_p_bt[0] == NUL)
	    return wp;

    return NULL;
}

/*
 * Go to a window in any tabpage containing the specified file.  Returns TRUE
 * if successfully jumped to the window. Otherwise returns FALSE.
 */
    static int
qf_goto_tabwin_with_file(int fnum)
{
    tabpage_T	*tp;
    win_T	*wp;

    FOR_ALL_TAB_WINDOWS(tp, wp)
	if (wp->w_buffer->b_fnum == fnum)
	{
	    goto_tabpage_win(tp, wp);
	    return TRUE;
	}

    return FALSE;
}

/*
 * Create a new window to show a file above the quickfix window. Called when
 * only the quickfix window is present.
 */
    static int
qf_open_new_file_win(qf_info_T *ll_ref)
{
    int		flags;

    flags = WSP_ABOVE;
    if (ll_ref != NULL)
	flags |= WSP_NEWLOC;
    if (win_split(0, flags) == FAIL)
	return FAIL;		/* not enough room for window */
    p_swb = empty_option;	/* don't split again */
    swb_flags = 0;
    RESET_BINDING(curwin);
    if (ll_ref != NULL)
    {
	/* The new window should use the location list from the
	 * location list window */
	curwin->w_llist = ll_ref;
	ll_ref->qf_refcount++;
    }
    return OK;
}

/*
 * Go to a window that shows the right buffer. If the window is not found, go
 * to the window just above the location list window. This is used for opening
 * a file from a location window and not from a quickfix window. If some usable
 * window is previously found, then it is supplied in 'use_win'.
 */
    static void
qf_goto_win_with_ll_file(win_T *use_win, int qf_fnum, qf_info_T *ll_ref)
{
    win_T	*win = use_win;

    if (win == NULL)
    {
	/* Find the window showing the selected file */
	FOR_ALL_WINDOWS(win)
	    if (win->w_buffer->b_fnum == qf_fnum)
		break;
	if (win == NULL)
	{
	    /* Find a previous usable window */
	    win = curwin;
	    do
	    {
		if (win->w_buffer->b_p_bt[0] == NUL)
		    break;
		if (win->w_prev == NULL)
		    win = lastwin;	/* wrap around the top */
		else
		    win = win->w_prev; /* go to previous window */
	    } while (win != curwin);
	}
    }
    win_goto(win);

    /* If the location list for the window is not set, then set it
     * to the location list from the location window */
    if (win->w_llist == NULL)
    {
	win->w_llist = ll_ref;
	ll_ref->qf_refcount++;
    }
}

/*
 * Go to a window that shows the specified file. If a window is not found, go
 * to the window just above the quickfix window. This is used for opening a
 * file from a quickfix window and not from a location window.
 */
    static void
qf_goto_win_with_qfl_file(int qf_fnum)
{
    win_T	*win;
    win_T	*altwin;

    win = curwin;
    altwin = NULL;
    for (;;)
    {
	if (win->w_buffer->b_fnum == qf_fnum)
	    break;
	if (win->w_prev == NULL)
	    win = lastwin;	/* wrap around the top */
	else
	    win = win->w_prev;	/* go to previous window */

	if (IS_QF_WINDOW(win))
	{
	    /* Didn't find it, go to the window before the quickfix
	     * window. */
	    if (altwin != NULL)
		win = altwin;
	    else if (curwin->w_prev != NULL)
		win = curwin->w_prev;
	    else
		win = curwin->w_next;
	    break;
	}

	/* Remember a usable window. */
	if (altwin == NULL && !win->w_p_pvw
		&& win->w_buffer->b_p_bt[0] == NUL)
	    altwin = win;
    }

    win_goto(win);
}

/*
 * Find a suitable window for opening a file (qf_fnum) from the
 * quickfix/location list and jump to it.  If the file is already opened in a
 * window, jump to it. Otherwise open a new window to display the file. This is
 * called from either a quickfix or a location list window.
 */
    static int
qf_jump_to_usable_window(int qf_fnum, int *opened_window)
{
    win_T	*usable_win_ptr = NULL;
    int		usable_win;
    qf_info_T	*ll_ref;
    win_T	*win;

    usable_win = 0;

    ll_ref = curwin->w_llist_ref;
    if (ll_ref != NULL)
    {
	/* Find a non-quickfix window with this location list */
	usable_win_ptr = qf_find_win_with_loclist(ll_ref);
	if (usable_win_ptr != NULL)
	    usable_win = 1;
    }

    if (!usable_win)
    {
	/* Locate a window showing a normal buffer */
	win = qf_find_win_with_normal_buf();
	if (win != NULL)
	    usable_win = 1;
    }

    /*
     * If no usable window is found and 'switchbuf' contains "usetab"
     * then search in other tabs.
     */
    if (!usable_win && (swb_flags & SWB_USETAB))
	usable_win = qf_goto_tabwin_with_file(qf_fnum);

    /*
     * If there is only one window and it is the quickfix window, create a
     * new one above the quickfix window.
     */
    if ((ONE_WINDOW && bt_quickfix(curbuf)) || !usable_win)
    {
	if (qf_open_new_file_win(ll_ref) != OK)
	    return FAIL;
	*opened_window = TRUE;	/* close it when fail */
    }
    else
    {
	if (curwin->w_llist_ref != NULL)	/* In a location window */
	    qf_goto_win_with_ll_file(usable_win_ptr, qf_fnum, ll_ref);
	else					/* In a quickfix window */
	    qf_goto_win_with_qfl_file(qf_fnum);
    }

    return OK;
}

/*
 * Edit the selected file or help file.
 */
    static int
qf_jump_edit_buffer(
	qf_info_T	*qi,
	qfline_T	*qf_ptr,
	int		forceit,
	win_T		*oldwin,
	int		*opened_window,
	int		*abort)
{
    int		retval = OK;

    if (qf_ptr->qf_type == 1)
    {
	/* Open help file (do_ecmd() will set b_help flag, readfile() will
	 * set b_p_ro flag). */
	if (!can_abandon(curbuf, forceit))
	{
	    no_write_message();
	    retval = FAIL;
	}
	else
	    retval = do_ecmd(qf_ptr->qf_fnum, NULL, NULL, NULL, (linenr_T)1,
		    ECMD_HIDE + ECMD_SET_HELP,
		    oldwin == curwin ? curwin : NULL);
    }
    else
    {
	int old_qf_curlist = qi->qf_curlist;
	int save_qfid = qi->qf_lists[qi->qf_curlist].qf_id;

	retval = buflist_getfile(qf_ptr->qf_fnum,
		(linenr_T)1, GETF_SETMARK | GETF_SWITCH, forceit);

	if (qi != &ql_info)
	{
	    /*
	     * Location list. Check whether the associated window is still
	     * present and the list is still valid.
	     */
	    if (!win_valid_any_tab(oldwin))
	    {
		EMSG(_("E924: Current window was closed"));
		*abort = TRUE;
		*opened_window = FALSE;
	    }
	    else if (!qflist_valid(oldwin, save_qfid))
	    {
		EMSG(_(e_loc_list_changed));
		*abort = TRUE;
	    }
	}
	else if (old_qf_curlist != qi->qf_curlist
		|| !is_qf_entry_present(qi, qf_ptr))
	{
	    if (qi == &ql_info)
		EMSG(_("E925: Current quickfix was changed"));
	    else
		EMSG(_(e_loc_list_changed));
	    *abort = TRUE;
	}

	if (*abort)
	    retval = FAIL;
    }

    return retval;
}

/*
 * Go to the error line in the current file using either line/column number or
 * a search pattern.
 */
    static void
qf_jump_goto_line(
	linenr_T	qf_lnum,
	int		qf_col,
	char_u		qf_viscol,
	char_u		*qf_pattern)
{
    linenr_T		i;
    char_u		*line;
    colnr_T		screen_col;
    colnr_T		char_col;

    if (qf_pattern == NULL)
    {
	/*
	 * Go to line with error, unless qf_lnum is 0.
	 */
	i = qf_lnum;
	if (i > 0)
	{
	    if (i > curbuf->b_ml.ml_line_count)
		i = curbuf->b_ml.ml_line_count;
	    curwin->w_cursor.lnum = i;
	}
	if (qf_col > 0)
	{
	    curwin->w_cursor.col = qf_col - 1;
#ifdef FEAT_VIRTUALEDIT
	    curwin->w_cursor.coladd = 0;
#endif
	    if (qf_viscol == TRUE)
	    {
		/*
		 * Check each character from the beginning of the error
		 * line up to the error column.  For each tab character
		 * found, reduce the error column value by the length of
		 * a tab character.
		 */
		line = ml_get_curline();
		screen_col = 0;
		for (char_col = 0; char_col < curwin->w_cursor.col; ++char_col)
		{
		    if (*line == NUL)
			break;
		    if (*line++ == '\t')
		    {
			curwin->w_cursor.col -= 7 - (screen_col % 8);
			screen_col += 8 - (screen_col % 8);
		    }
		    else
			++screen_col;
		}
	    }
	    check_cursor();
	}
	else
	    beginline(BL_WHITE | BL_FIX);
    }
    else
    {
	pos_T save_cursor;

	/* Move the cursor to the first line in the buffer */
	save_cursor = curwin->w_cursor;
	curwin->w_cursor.lnum = 0;
	if (!do_search(NULL, '/', qf_pattern, (long)1,
		    SEARCH_KEEP, NULL, NULL))
	    curwin->w_cursor = save_cursor;
    }
}

/*
 * Display quickfix list index and size message
 */
    static void
qf_jump_print_msg(
	qf_info_T	*qi,
	int		qf_index,
	qfline_T	*qf_ptr,
	buf_T		*old_curbuf,
	linenr_T	old_lnum)
{
    linenr_T		i;
    int			len;

    /* Update the screen before showing the message, unless the screen
     * scrolled up. */
    if (!msg_scrolled)
	update_topline_redraw();
    sprintf((char *)IObuff, _("(%d of %d)%s%s: "), qf_index,
	    qi->qf_lists[qi->qf_curlist].qf_count,
	    qf_ptr->qf_cleared ? _(" (line deleted)") : "",
	    (char *)qf_types(qf_ptr->qf_type, qf_ptr->qf_nr));
    /* Add the message, skipping leading whitespace and newlines. */
    len = (int)STRLEN(IObuff);
    qf_fmt_text(skipwhite(qf_ptr->qf_text), IObuff + len, IOSIZE - len);

    /* Output the message.  Overwrite to avoid scrolling when the 'O'
     * flag is present in 'shortmess'; But when not jumping, print the
     * whole message. */
    i = msg_scroll;
    if (curbuf == old_curbuf && curwin->w_cursor.lnum == old_lnum)
	msg_scroll = TRUE;
    else if (!msg_scrolled && shortmess(SHM_OVERALL))
	msg_scroll = FALSE;
    msg_attr_keep(IObuff, 0, TRUE);
    msg_scroll = i;
}

/*
 * jump to a quickfix line
 * if dir == FORWARD go "errornr" valid entries forward
 * if dir == BACKWARD go "errornr" valid entries backward
 * if dir == FORWARD_FILE go "errornr" valid entries files backward
 * if dir == BACKWARD_FILE go "errornr" valid entries files backward
 * else if "errornr" is zero, redisplay the same line
 * else go to entry "errornr"
 */
    void
qf_jump(qf_info_T	*qi,
	int		dir,
	int		errornr,
	int		forceit)
{
    qfline_T		*qf_ptr;
    qfline_T		*old_qf_ptr;
    int			qf_index;
    int			old_qf_index;
    buf_T		*old_curbuf;
    linenr_T		old_lnum;
    char_u		*old_swb = p_swb;
    unsigned		old_swb_flags = swb_flags;
    int			opened_window = FALSE;
    win_T		*oldwin = curwin;
    int			print_message = TRUE;
#ifdef FEAT_FOLDING
    int			old_KeyTyped = KeyTyped; /* getting file may reset it */
#endif
    int			retval = OK;

    if (qi == NULL)
	qi = &ql_info;

    if (qi->qf_curlist >= qi->qf_listcount
	|| qi->qf_lists[qi->qf_curlist].qf_count == 0)
    {
	EMSG(_(e_quickfix));
	return;
    }

    qf_ptr = qi->qf_lists[qi->qf_curlist].qf_ptr;
    old_qf_ptr = qf_ptr;
    qf_index = qi->qf_lists[qi->qf_curlist].qf_index;
    old_qf_index = qf_index;
    if (dir != 0)    /* next/prev valid entry */
    {
	qf_ptr = get_nth_valid_entry(qi, errornr, qf_ptr, &qf_index, dir);
	if (qf_ptr == NULL)
	{
	    qf_ptr = old_qf_ptr;
	    qf_index = old_qf_index;
	    goto theend;
	}
    }
    else if (errornr != 0)	/* go to specified number */
	qf_ptr = get_nth_entry(qi, errornr, qf_ptr, &qf_index);

    qi->qf_lists[qi->qf_curlist].qf_index = qf_index;
    if (qf_win_pos_update(qi, old_qf_index))
	/* No need to print the error message if it's visible in the error
	 * window */
	print_message = FALSE;

    /*
     * For ":helpgrep" find a help window or open one.
     */
    if (qf_ptr->qf_type == 1 && (!bt_help(curwin->w_buffer) || cmdmod.tab != 0))
	if (jump_to_help_window(qi, &opened_window) == FAIL)
	    goto theend;

    /*
     * If currently in the quickfix window, find another window to show the
     * file in.
     */
    if (bt_quickfix(curbuf) && !opened_window)
    {
	/*
	 * If there is no file specified, we don't know where to go.
	 * But do advance, otherwise ":cn" gets stuck.
	 */
	if (qf_ptr->qf_fnum == 0)
	    goto theend;

	if (qf_jump_to_usable_window(qf_ptr->qf_fnum, &opened_window) == FAIL)
	    goto failed;
    }

    /*
     * If there is a file name,
     * read the wanted file if needed, and check autowrite etc.
     */
    old_curbuf = curbuf;
    old_lnum = curwin->w_cursor.lnum;

    if (qf_ptr->qf_fnum != 0)
    {
	int abort = FALSE;

	retval = qf_jump_edit_buffer(qi, qf_ptr, forceit, oldwin,
						&opened_window, &abort);
	if (abort)
	{
	    qi = NULL;
	    qf_ptr = NULL;
	}
    }

    if (retval == OK)
    {
	/* When not switched to another buffer, still need to set pc mark */
	if (curbuf == old_curbuf)
	    setpcmark();

	qf_jump_goto_line(qf_ptr->qf_lnum, qf_ptr->qf_col, qf_ptr->qf_viscol,
							qf_ptr->qf_pattern);

#ifdef FEAT_FOLDING
	if ((fdo_flags & FDO_QUICKFIX) && old_KeyTyped)
	    foldOpenCursor();
#endif
	if (print_message)
	    qf_jump_print_msg(qi, qf_index, qf_ptr, old_curbuf, old_lnum);
    }
    else
    {
	if (opened_window)
	    win_close(curwin, TRUE);    /* Close opened window */
	if (qf_ptr != NULL && qf_ptr->qf_fnum != 0)
	{
	    /*
	     * Couldn't open file, so put index back where it was.  This could
	     * happen if the file was readonly and we changed something.
	     */
failed:
	    qf_ptr = old_qf_ptr;
	    qf_index = old_qf_index;
	}
    }
theend:
    if (qi != NULL)
    {
	qi->qf_lists[qi->qf_curlist].qf_ptr = qf_ptr;
	qi->qf_lists[qi->qf_curlist].qf_index = qf_index;
    }
    if (p_swb != old_swb && opened_window)
    {
	/* Restore old 'switchbuf' value, but not when an autocommand or
	 * modeline has changed the value. */
	if (p_swb == empty_option)
	{
	    p_swb = old_swb;
	    swb_flags = old_swb_flags;
	}
	else
	    free_string_option(old_swb);
    }
}

/*
 * ":clist": list all errors
 * ":llist": list all locations
 */
    void
qf_list(exarg_T *eap)
{
    buf_T	*buf;
    char_u	*fname;
    qfline_T	*qfp;
    int		i;
    int		idx1 = 1;
    int		idx2 = -1;
    char_u	*arg = eap->arg;
    int		plus = FALSE;
    int		qfFileAttr;
    int		qfSepAttr;
    int		qfLineAttr;
    int		all = eap->forceit;	/* if not :cl!, only show
						   recognised errors */
    qf_info_T	*qi = &ql_info;

    if (eap->cmdidx == CMD_llist)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	{
	    EMSG(_(e_loclist));
	    return;
	}
    }

    if (qi->qf_curlist >= qi->qf_listcount
	|| qi->qf_lists[qi->qf_curlist].qf_count == 0)
    {
	EMSG(_(e_quickfix));
	return;
    }
    if (*arg == '+')
    {
	++arg;
	plus = TRUE;
    }
    if (!get_list_range(&arg, &idx1, &idx2) || *arg != NUL)
    {
	EMSG(_(e_trailing));
	return;
    }
    if (plus)
    {
	i = qi->qf_lists[qi->qf_curlist].qf_index;
	idx2 = i + idx1;
	idx1 = i;
    }
    else
    {
	i = qi->qf_lists[qi->qf_curlist].qf_count;
	if (idx1 < 0)
	    idx1 = (-idx1 > i) ? 0 : idx1 + i + 1;
	if (idx2 < 0)
	    idx2 = (-idx2 > i) ? 0 : idx2 + i + 1;
    }

    /* Shorten all the file names, so that it is easy to read */
    shorten_fnames(FALSE);

    /*
     * Get the attributes for the different quickfix highlight items.  Note
     * that this depends on syntax items defined in the qf.vim syntax file
     */
    qfFileAttr = syn_name2attr((char_u *)"qfFileName");
    if (qfFileAttr == 0)
	qfFileAttr = HL_ATTR(HLF_D);
    qfSepAttr = syn_name2attr((char_u *)"qfSeparator");
    if (qfSepAttr == 0)
	qfSepAttr = HL_ATTR(HLF_D);
    qfLineAttr = syn_name2attr((char_u *)"qfLineNr");
    if (qfLineAttr == 0)
	qfLineAttr = HL_ATTR(HLF_N);

    if (qi->qf_lists[qi->qf_curlist].qf_nonevalid)
	all = TRUE;
    qfp = qi->qf_lists[qi->qf_curlist].qf_start;
    for (i = 1; !got_int && i <= qi->qf_lists[qi->qf_curlist].qf_count; )
    {
	if ((qfp->qf_valid || all) && idx1 <= i && i <= idx2)
	{
	    msg_putchar('\n');
	    if (got_int)
		break;

	    fname = NULL;
	    if (qfp->qf_module != NULL && *qfp->qf_module != NUL)
		vim_snprintf((char *)IObuff, IOSIZE, "%2d %s", i, (char *)qfp->qf_module);
	    else {
		if (qfp->qf_fnum != 0
				&& (buf = buflist_findnr(qfp->qf_fnum)) != NULL)
		{
		    fname = buf->b_fname;
		    if (qfp->qf_type == 1)	/* :helpgrep */
			fname = gettail(fname);
		}
		if (fname == NULL)
		    sprintf((char *)IObuff, "%2d", i);
		else
		    vim_snprintf((char *)IObuff, IOSIZE, "%2d %s",
								i, (char *)fname);
	    }
	    msg_outtrans_attr(IObuff, i == qi->qf_lists[qi->qf_curlist].qf_index
					   ? HL_ATTR(HLF_QFL) : qfFileAttr);

	    if (qfp->qf_lnum != 0)
		msg_puts_attr((char_u *)":", qfSepAttr);
	    if (qfp->qf_lnum == 0)
		IObuff[0] = NUL;
	    else if (qfp->qf_col == 0)
		sprintf((char *)IObuff, "%ld", qfp->qf_lnum);
	    else
		sprintf((char *)IObuff, "%ld col %d",
						   qfp->qf_lnum, qfp->qf_col);
	    sprintf((char *)IObuff + STRLEN(IObuff), "%s",
				  (char *)qf_types(qfp->qf_type, qfp->qf_nr));
	    msg_puts_attr(IObuff, qfLineAttr);
	    msg_puts_attr((char_u *)":", qfSepAttr);
	    if (qfp->qf_pattern != NULL)
	    {
		qf_fmt_text(qfp->qf_pattern, IObuff, IOSIZE);
		msg_puts(IObuff);
		msg_puts_attr((char_u *)":", qfSepAttr);
	    }
	    msg_puts((char_u *)" ");

	    /* Remove newlines and leading whitespace from the text.  For an
	     * unrecognized line keep the indent, the compiler may mark a word
	     * with ^^^^. */
	    qf_fmt_text((fname != NULL || qfp->qf_lnum != 0)
				     ? skipwhite(qfp->qf_text) : qfp->qf_text,
							      IObuff, IOSIZE);
	    msg_prt_line(IObuff, FALSE);
	    out_flush();		/* show one line at a time */
	}

	qfp = qfp->qf_next;
	if (qfp == NULL)
	    break;
	++i;
	ui_breakcheck();
    }
}

/*
 * Remove newlines and leading whitespace from an error message.
 * Put the result in "buf[bufsize]".
 */
    static void
qf_fmt_text(char_u *text, char_u *buf, int bufsize)
{
    int		i;
    char_u	*p = text;

    for (i = 0; *p != NUL && i < bufsize - 1; ++i)
    {
	if (*p == '\n')
	{
	    buf[i] = ' ';
	    while (*++p != NUL)
		if (!VIM_ISWHITE(*p) && *p != '\n')
		    break;
	}
	else
	    buf[i] = *p++;
    }
    buf[i] = NUL;
}

/*
 * Display information (list number, list size and the title) about a
 * quickfix/location list.
 */
    static void
qf_msg(qf_info_T *qi, int which, char *lead)
{
    char   *title = (char *)qi->qf_lists[which].qf_title;
    int    count = qi->qf_lists[which].qf_count;
    char_u buf[IOSIZE];

    vim_snprintf((char *)buf, IOSIZE, _("%serror list %d of %d; %d errors "),
	    lead,
	    which + 1,
	    qi->qf_listcount,
	    count);

    if (title != NULL)
    {
	size_t	len = STRLEN(buf);

	if (len < 34)
	{
	    vim_memset(buf + len, ' ', 34 - len);
	    buf[34] = NUL;
	}
	vim_strcat(buf, (char_u *)title, IOSIZE);
    }
    trunc_string(buf, buf, Columns - 1, IOSIZE);
    msg(buf);
}

/*
 * ":colder [count]": Up in the quickfix stack.
 * ":cnewer [count]": Down in the quickfix stack.
 * ":lolder [count]": Up in the location list stack.
 * ":lnewer [count]": Down in the location list stack.
 */
    void
qf_age(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    int		count;

    if (eap->cmdidx == CMD_lolder || eap->cmdidx == CMD_lnewer)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	{
	    EMSG(_(e_loclist));
	    return;
	}
    }

    if (eap->addr_count != 0)
	count = eap->line2;
    else
	count = 1;
    while (count--)
    {
	if (eap->cmdidx == CMD_colder || eap->cmdidx == CMD_lolder)
	{
	    if (qi->qf_curlist == 0)
	    {
		EMSG(_("E380: At bottom of quickfix stack"));
		break;
	    }
	    --qi->qf_curlist;
	}
	else
	{
	    if (qi->qf_curlist >= qi->qf_listcount - 1)
	    {
		EMSG(_("E381: At top of quickfix stack"));
		break;
	    }
	    ++qi->qf_curlist;
	}
    }
    qf_msg(qi, qi->qf_curlist, "");
    qf_update_buffer(qi, NULL);
}

/*
 * Display the information about all the quickfix/location lists in the stack
 */
    void
qf_history(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    int		i;

    if (eap->cmdidx == CMD_lhistory)
	qi = GET_LOC_LIST(curwin);
    if (qi == NULL || (qi->qf_listcount == 0
				&& qi->qf_lists[qi->qf_curlist].qf_count == 0))
	MSG(_("No entries"));
    else
	for (i = 0; i < qi->qf_listcount; ++i)
	    qf_msg(qi, i, i == qi->qf_curlist ? "> " : "  ");
}

/*
 * Free all the entries in the error list "idx". Note that other information
 * associated with the list like context and title are not freed.
 */
    static void
qf_free_items(qf_info_T *qi, int idx)
{
    qfline_T	*qfp;
    qfline_T	*qfpnext;
    int		stop = FALSE;
    qf_list_T	*qfl = &qi->qf_lists[idx];

    while (qfl->qf_count && qfl->qf_start != NULL)
    {
	qfp = qfl->qf_start;
	qfpnext = qfp->qf_next;
	if (!stop)
	{
	    vim_free(qfp->qf_module);
	    vim_free(qfp->qf_text);
	    vim_free(qfp->qf_pattern);
	    stop = (qfp == qfpnext);
	    vim_free(qfp);
	    if (stop)
		/* Somehow qf_count may have an incorrect value, set it to 1
		 * to avoid crashing when it's wrong.
		 * TODO: Avoid qf_count being incorrect. */
		qfl->qf_count = 1;
	}
	qfl->qf_start = qfpnext;
	--qfl->qf_count;
    }

    qfl->qf_index = 0;
    qfl->qf_start = NULL;
    qfl->qf_last = NULL;
    qfl->qf_ptr = NULL;
    qfl->qf_nonevalid = TRUE;

    qf_clean_dir_stack(&qfl->qf_dir_stack);
    qfl->qf_directory = NULL;
    qf_clean_dir_stack(&qfl->qf_file_stack);
    qfl->qf_currfile = NULL;
    qfl->qf_multiline = FALSE;
    qfl->qf_multiignore = FALSE;
    qfl->qf_multiscan = FALSE;
}

/*
 * Free error list "idx". Frees all the entries in the quickfix list,
 * associated context information and the title.
 */
    static void
qf_free(qf_info_T *qi, int idx)
{
    qf_list_T	*qfl = &qi->qf_lists[idx];

    qf_free_items(qi, idx);

    VIM_CLEAR(qfl->qf_title);
    free_tv(qfl->qf_ctx);
    qfl->qf_ctx = NULL;
    qfl->qf_id = 0;
    qfl->qf_changedtick = 0L;
}

/*
 * qf_mark_adjust: adjust marks
 */
   void
qf_mark_adjust(
	win_T	*wp,
	linenr_T	line1,
	linenr_T	line2,
	long	amount,
	long	amount_after)
{
    int		i;
    qfline_T	*qfp;
    int		idx;
    qf_info_T	*qi = &ql_info;
    int		found_one = FALSE;
    int		buf_has_flag = wp == NULL ? BUF_HAS_QF_ENTRY : BUF_HAS_LL_ENTRY;

    if (!(curbuf->b_has_qf_entry & buf_has_flag))
	return;
    if (wp != NULL)
    {
	if (wp->w_llist == NULL)
	    return;
	qi = wp->w_llist;
    }

    for (idx = 0; idx < qi->qf_listcount; ++idx)
	if (qi->qf_lists[idx].qf_count)
	    for (i = 0, qfp = qi->qf_lists[idx].qf_start;
			i < qi->qf_lists[idx].qf_count && qfp != NULL;
			++i, qfp = qfp->qf_next)
		if (qfp->qf_fnum == curbuf->b_fnum)
		{
		    found_one = TRUE;
		    if (qfp->qf_lnum >= line1 && qfp->qf_lnum <= line2)
		    {
			if (amount == MAXLNUM)
			    qfp->qf_cleared = TRUE;
			else
			    qfp->qf_lnum += amount;
		    }
		    else if (amount_after && qfp->qf_lnum > line2)
			qfp->qf_lnum += amount_after;
		}

    if (!found_one)
	curbuf->b_has_qf_entry &= ~buf_has_flag;
}

/*
 * Make a nice message out of the error character and the error number:
 *  char    number	message
 *  e or E    0		" error"
 *  w or W    0		" warning"
 *  i or I    0		" info"
 *  0	      0		""
 *  other     0		" c"
 *  e or E    n		" error n"
 *  w or W    n		" warning n"
 *  i or I    n		" info n"
 *  0	      n		" error n"
 *  other     n		" c n"
 *  1	      x		""	:helpgrep
 */
    static char_u *
qf_types(int c, int nr)
{
    static char_u	buf[20];
    static char_u	cc[3];
    char_u		*p;

    if (c == 'W' || c == 'w')
	p = (char_u *)" warning";
    else if (c == 'I' || c == 'i')
	p = (char_u *)" info";
    else if (c == 'E' || c == 'e' || (c == 0 && nr > 0))
	p = (char_u *)" error";
    else if (c == 0 || c == 1)
	p = (char_u *)"";
    else
    {
	cc[0] = ' ';
	cc[1] = c;
	cc[2] = NUL;
	p = cc;
    }

    if (nr <= 0)
	return p;

    sprintf((char *)buf, "%s %3d", (char *)p, nr);
    return buf;
}

/*
 * ":cwindow": open the quickfix window if we have errors to display,
 *	       close it if not.
 * ":lwindow": open the location list window if we have locations to display,
 *	       close it if not.
 */
    void
ex_cwindow(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    win_T	*win;

    if (eap->cmdidx == CMD_lwindow)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	    return;
    }

    /* Look for an existing quickfix window.  */
    win = qf_find_win(qi);

    /*
     * If a quickfix window is open but we have no errors to display,
     * close the window.  If a quickfix window is not open, then open
     * it if we have errors; otherwise, leave it closed.
     */
    if (qi->qf_lists[qi->qf_curlist].qf_nonevalid
	    || qi->qf_lists[qi->qf_curlist].qf_count == 0
	    || qi->qf_curlist >= qi->qf_listcount)
    {
	if (win != NULL)
	    ex_cclose(eap);
    }
    else if (win == NULL)
	ex_copen(eap);
}

/*
 * ":cclose": close the window showing the list of errors.
 * ":lclose": close the window showing the location list
 */
    void
ex_cclose(exarg_T *eap)
{
    win_T	*win = NULL;
    qf_info_T	*qi = &ql_info;

    if (eap->cmdidx == CMD_lclose || eap->cmdidx == CMD_lwindow)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	    return;
    }

    /* Find existing quickfix window and close it. */
    win = qf_find_win(qi);
    if (win != NULL)
	win_close(win, FALSE);
}

/*
 * ":copen": open a window that shows the list of errors.
 * ":lopen": open a window that shows the location list.
 */
    void
ex_copen(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    int		height;
    win_T	*win;
    tabpage_T	*prevtab = curtab;
    buf_T	*qf_buf;
    win_T	*oldwin = curwin;

    if (eap->cmdidx == CMD_lopen || eap->cmdidx == CMD_lwindow)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	{
	    EMSG(_(e_loclist));
	    return;
	}
    }

    if (eap->addr_count != 0)
	height = eap->line2;
    else
	height = QF_WINHEIGHT;

    reset_VIsual_and_resel();			/* stop Visual mode */
#ifdef FEAT_GUI
    need_mouse_correct = TRUE;
#endif

    /*
     * Find existing quickfix window, or open a new one.
     */
    win = qf_find_win(qi);

    if (win != NULL && cmdmod.tab == 0)
    {
	win_goto(win);
	if (eap->addr_count != 0)
	{
	    if (cmdmod.split & WSP_VERT)
	    {
		if (height != win->w_width)
		    win_setwidth(height);
	    }
	    else if (height != win->w_height)
		win_setheight(height);
	}
    }
    else
    {
	int flags = 0;

	qf_buf = qf_find_buf(qi);

	/* The current window becomes the previous window afterwards. */
	win = curwin;

	if ((eap->cmdidx == CMD_copen || eap->cmdidx == CMD_cwindow)
		&& cmdmod.split == 0)
	    /* Create the new quickfix window at the very bottom, except when
	     * :belowright or :aboveleft is used. */
	    win_goto(lastwin);
	/* Default is to open the window below the current window */
	if (cmdmod.split == 0)
	    flags = WSP_BELOW;
	flags |= WSP_NEWLOC;
	if (win_split(height, flags) == FAIL)
	    return;		/* not enough room for window */
	RESET_BINDING(curwin);

	if (eap->cmdidx == CMD_lopen || eap->cmdidx == CMD_lwindow)
	{
	    /*
	     * For the location list window, create a reference to the
	     * location list from the window 'win'.
	     */
	    curwin->w_llist_ref = win->w_llist;
	    win->w_llist->qf_refcount++;
	}

	if (oldwin != curwin)
	    oldwin = NULL;  /* don't store info when in another window */
	if (qf_buf != NULL)
	    /* Use the existing quickfix buffer */
	    (void)do_ecmd(qf_buf->b_fnum, NULL, NULL, NULL, ECMD_ONE,
					     ECMD_HIDE + ECMD_OLDBUF, oldwin);
	else
	{
	    /* Create a new quickfix buffer */
	    (void)do_ecmd(0, NULL, NULL, NULL, ECMD_ONE, ECMD_HIDE, oldwin);
	    /* switch off 'swapfile' */
	    set_option_value((char_u *)"swf", 0L, NULL, OPT_LOCAL);
	    set_option_value((char_u *)"bt", 0L, (char_u *)"quickfix",
								   OPT_LOCAL);
	    set_option_value((char_u *)"bh", 0L, (char_u *)"wipe", OPT_LOCAL);
	    RESET_BINDING(curwin);
#ifdef FEAT_DIFF
	    curwin->w_p_diff = FALSE;
#endif
#ifdef FEAT_FOLDING
	    set_option_value((char_u *)"fdm", 0L, (char_u *)"manual",
								   OPT_LOCAL);
#endif
	}

	/* Only set the height when still in the same tab page and there is no
	 * window to the side. */
	if (curtab == prevtab && curwin->w_width == Columns)
	    win_setheight(height);
	curwin->w_p_wfh = TRUE;	    /* set 'winfixheight' */
	if (win_valid(win))
	    prevwin = win;
    }

    qf_set_title_var(qi);

    /*
     * Fill the buffer with the quickfix list.
     */
    qf_fill_buffer(qi, curbuf, NULL);

    curwin->w_cursor.lnum = qi->qf_lists[qi->qf_curlist].qf_index;
    curwin->w_cursor.col = 0;
    check_cursor();
    update_topline();		/* scroll to show the line */
}

/*
 * Move the cursor in the quickfix window to "lnum".
 */
    static void
qf_win_goto(win_T *win, linenr_T lnum)
{
    win_T	*old_curwin = curwin;

    curwin = win;
    curbuf = win->w_buffer;
    curwin->w_cursor.lnum = lnum;
    curwin->w_cursor.col = 0;
#ifdef FEAT_VIRTUALEDIT
    curwin->w_cursor.coladd = 0;
#endif
    curwin->w_curswant = 0;
    update_topline();		/* scroll to show the line */
    redraw_later(VALID);
    curwin->w_redr_status = TRUE;	/* update ruler */
    curwin = old_curwin;
    curbuf = curwin->w_buffer;
}

/*
 * :cbottom/:lbottom commands.
 */
    void
ex_cbottom(exarg_T *eap UNUSED)
{
    qf_info_T	*qi = &ql_info;
    win_T	*win;

    if (eap->cmdidx == CMD_lbottom)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	{
	    EMSG(_(e_loclist));
	    return;
	}
    }

    win = qf_find_win(qi);
    if (win != NULL && win->w_cursor.lnum != win->w_buffer->b_ml.ml_line_count)
	qf_win_goto(win, win->w_buffer->b_ml.ml_line_count);
}

/*
 * Return the number of the current entry (line number in the quickfix
 * window).
 */
     linenr_T
qf_current_entry(win_T *wp)
{
    qf_info_T	*qi = &ql_info;

    if (IS_LL_WINDOW(wp))
	/* In the location list window, use the referenced location list */
	qi = wp->w_llist_ref;

    return qi->qf_lists[qi->qf_curlist].qf_index;
}

/*
 * Update the cursor position in the quickfix window to the current error.
 * Return TRUE if there is a quickfix window.
 */
    static int
qf_win_pos_update(
    qf_info_T	*qi,
    int		old_qf_index)	/* previous qf_index or zero */
{
    win_T	*win;
    int		qf_index = qi->qf_lists[qi->qf_curlist].qf_index;

    /*
     * Put the cursor on the current error in the quickfix window, so that
     * it's viewable.
     */
    win = qf_find_win(qi);
    if (win != NULL
	    && qf_index <= win->w_buffer->b_ml.ml_line_count
	    && old_qf_index != qf_index)
    {
	if (qf_index > old_qf_index)
	{
	    win->w_redraw_top = old_qf_index;
	    win->w_redraw_bot = qf_index;
	}
	else
	{
	    win->w_redraw_top = qf_index;
	    win->w_redraw_bot = old_qf_index;
	}
	qf_win_goto(win, qf_index);
    }
    return win != NULL;
}

/*
 * Check whether the given window is displaying the specified quickfix/location
 * list buffer
 */
    static int
is_qf_win(win_T *win, qf_info_T *qi)
{
    /*
     * A window displaying the quickfix buffer will have the w_llist_ref field
     * set to NULL.
     * A window displaying a location list buffer will have the w_llist_ref
     * pointing to the location list.
     */
    if (bt_quickfix(win->w_buffer))
	if ((qi == &ql_info && win->w_llist_ref == NULL)
		|| (qi != &ql_info && win->w_llist_ref == qi))
	    return TRUE;

    return FALSE;
}

/*
 * Find a window displaying the quickfix/location list 'qi'
 * Only searches in the current tabpage.
 */
    static win_T *
qf_find_win(qf_info_T *qi)
{
    win_T	*win;

    FOR_ALL_WINDOWS(win)
	if (is_qf_win(win, qi))
	    return win;
    return NULL;
}

/*
 * Find a quickfix buffer.
 * Searches in windows opened in all the tabs.
 */
    static buf_T *
qf_find_buf(qf_info_T *qi)
{
    tabpage_T	*tp;
    win_T	*win;

    FOR_ALL_TAB_WINDOWS(tp, win)
	if (is_qf_win(win, qi))
	    return win->w_buffer;

    return NULL;
}

/*
 * Update the w:quickfix_title variable in the quickfix/location list window
 */
    static void
qf_update_win_titlevar(qf_info_T *qi)
{
    win_T	*win;
    win_T	*curwin_save;

    if ((win = qf_find_win(qi)) != NULL)
    {
	curwin_save = curwin;
	curwin = win;
	qf_set_title_var(qi);
	curwin = curwin_save;
    }
}

/*
 * Find the quickfix buffer.  If it exists, update the contents.
 */
    static void
qf_update_buffer(qf_info_T *qi, qfline_T *old_last)
{
    buf_T	*buf;
    win_T	*win;
    aco_save_T	aco;

    /* Check if a buffer for the quickfix list exists.  Update it. */
    buf = qf_find_buf(qi);
    if (buf != NULL)
    {
	linenr_T	old_line_count = buf->b_ml.ml_line_count;

	if (old_last == NULL)
	    /* set curwin/curbuf to buf and save a few things */
	    aucmd_prepbuf(&aco, buf);

	qf_update_win_titlevar(qi);

	qf_fill_buffer(qi, buf, old_last);
	++CHANGEDTICK(buf);

	if (old_last == NULL)
	{
	    (void)qf_win_pos_update(qi, 0);

	    /* restore curwin/curbuf and a few other things */
	    aucmd_restbuf(&aco);
	}

	/* Only redraw when added lines are visible.  This avoids flickering
	 * when the added lines are not visible. */
	if ((win = qf_find_win(qi)) != NULL && old_line_count < win->w_botline)
	    redraw_buf_later(buf, NOT_VALID);
    }
}

/*
 * Set "w:quickfix_title" if "qi" has a title.
 */
    static void
qf_set_title_var(qf_info_T *qi)
{
    if (qi->qf_lists[qi->qf_curlist].qf_title != NULL)
	set_internal_string_var((char_u *)"w:quickfix_title",
				    qi->qf_lists[qi->qf_curlist].qf_title);
}

/*
 * Fill current buffer with quickfix errors, replacing any previous contents.
 * curbuf must be the quickfix buffer!
 * If "old_last" is not NULL append the items after this one.
 * When "old_last" is NULL then "buf" must equal "curbuf"!  Because
 * ml_delete() is used and autocommands will be triggered.
 */
    static void
qf_fill_buffer(qf_info_T *qi, buf_T *buf, qfline_T *old_last)
{
    linenr_T	lnum;
    qfline_T	*qfp;
    buf_T	*errbuf;
    int		len;
    int		old_KeyTyped = KeyTyped;

    if (old_last == NULL)
    {
	if (buf != curbuf)
	{
	    internal_error("qf_fill_buffer()");
	    return;
	}

	/* delete all existing lines */
	while ((curbuf->b_ml.ml_flags & ML_EMPTY) == 0)
	    (void)ml_delete((linenr_T)1, FALSE);
    }

    /* Check if there is anything to display */
    if (qi->qf_curlist < qi->qf_listcount)
    {
	char_u	dirname[MAXPATHL];

	*dirname = NUL;

	/* Add one line for each error */
	if (old_last == NULL)
	{
	    qfp = qi->qf_lists[qi->qf_curlist].qf_start;
	    lnum = 0;
	}
	else
	{
	    qfp = old_last->qf_next;
	    lnum = buf->b_ml.ml_line_count;
	}
	while (lnum < qi->qf_lists[qi->qf_curlist].qf_count)
	{
	    if (qfp->qf_module != NULL)
	    {
		STRCPY(IObuff, qfp->qf_module);
		len = (int)STRLEN(IObuff);
	    }
	    else if (qfp->qf_fnum != 0
		    && (errbuf = buflist_findnr(qfp->qf_fnum)) != NULL
		    && errbuf->b_fname != NULL)
	    {
		if (qfp->qf_type == 1)	/* :helpgrep */
		    STRCPY(IObuff, gettail(errbuf->b_fname));
		else
		{
		    /* shorten the file name if not done already */
		    if (errbuf->b_sfname == NULL
					   || mch_isFullName(errbuf->b_sfname))
		    {
			if (*dirname == NUL)
			    mch_dirname(dirname, MAXPATHL);
			shorten_buf_fname(errbuf, dirname, FALSE);
		    }
		    STRCPY(IObuff, errbuf->b_fname);
		}
		len = (int)STRLEN(IObuff);
	    }
	    else
		len = 0;
	    IObuff[len++] = '|';

	    if (qfp->qf_lnum > 0)
	    {
		sprintf((char *)IObuff + len, "%ld", qfp->qf_lnum);
		len += (int)STRLEN(IObuff + len);

		if (qfp->qf_col > 0)
		{
		    sprintf((char *)IObuff + len, " col %d", qfp->qf_col);
		    len += (int)STRLEN(IObuff + len);
		}

		sprintf((char *)IObuff + len, "%s",
				  (char *)qf_types(qfp->qf_type, qfp->qf_nr));
		len += (int)STRLEN(IObuff + len);
	    }
	    else if (qfp->qf_pattern != NULL)
	    {
		qf_fmt_text(qfp->qf_pattern, IObuff + len, IOSIZE - len);
		len += (int)STRLEN(IObuff + len);
	    }
	    IObuff[len++] = '|';
	    IObuff[len++] = ' ';

	    /* Remove newlines and leading whitespace from the text.
	     * For an unrecognized line keep the indent, the compiler may
	     * mark a word with ^^^^. */
	    qf_fmt_text(len > 3 ? skipwhite(qfp->qf_text) : qfp->qf_text,
						  IObuff + len, IOSIZE - len);

	    if (ml_append_buf(buf, lnum, IObuff,
				  (colnr_T)STRLEN(IObuff) + 1, FALSE) == FAIL)
		break;
	    ++lnum;
	    qfp = qfp->qf_next;
	    if (qfp == NULL)
		break;
	}

	if (old_last == NULL)
	    /* Delete the empty line which is now at the end */
	    (void)ml_delete(lnum + 1, FALSE);
    }

    /* correct cursor position */
    check_lnums(TRUE);

    if (old_last == NULL)
    {
	/* Set the 'filetype' to "qf" each time after filling the buffer.
	 * This resembles reading a file into a buffer, it's more logical when
	 * using autocommands. */
	++curbuf_lock;
	set_option_value((char_u *)"ft", 0L, (char_u *)"qf", OPT_LOCAL);
	curbuf->b_p_ma = FALSE;

	keep_filetype = TRUE;		/* don't detect 'filetype' */
	apply_autocmds(EVENT_BUFREADPOST, (char_u *)"quickfix", NULL,
							       FALSE, curbuf);
	apply_autocmds(EVENT_BUFWINENTER, (char_u *)"quickfix", NULL,
							       FALSE, curbuf);
	keep_filetype = FALSE;
	--curbuf_lock;

	/* make sure it will be redrawn */
	redraw_curbuf_later(NOT_VALID);
    }

    /* Restore KeyTyped, setting 'filetype' may reset it. */
    KeyTyped = old_KeyTyped;
}

/*
 * For every change made to the quickfix list, update the changed tick.
 */
    static void
qf_list_changed(qf_info_T *qi, int qf_idx)
{
    qi->qf_lists[qf_idx].qf_changedtick++;
}

/*
 * Return TRUE when using ":vimgrep" for ":grep".
 */
    int
grep_internal(cmdidx_T cmdidx)
{
    return ((cmdidx == CMD_grep
		|| cmdidx == CMD_lgrep
		|| cmdidx == CMD_grepadd
		|| cmdidx == CMD_lgrepadd)
	    && STRCMP("internal",
			*curbuf->b_p_gp == NUL ? p_gp : curbuf->b_p_gp) == 0);
}

/*
 * Used for ":make", ":lmake", ":grep", ":lgrep", ":grepadd", and ":lgrepadd"
 */
    void
ex_make(exarg_T *eap)
{
    char_u	*fname;
    char_u	*cmd;
    char_u	*enc = NULL;
    unsigned	len;
    win_T	*wp = NULL;
    qf_info_T	*qi = &ql_info;
    int		res;
    char_u	*au_name = NULL;

    /* Redirect ":grep" to ":vimgrep" if 'grepprg' is "internal". */
    if (grep_internal(eap->cmdidx))
    {
	ex_vimgrep(eap);
	return;
    }

    switch (eap->cmdidx)
    {
	case CMD_make:	    au_name = (char_u *)"make"; break;
	case CMD_lmake:	    au_name = (char_u *)"lmake"; break;
	case CMD_grep:	    au_name = (char_u *)"grep"; break;
	case CMD_lgrep:	    au_name = (char_u *)"lgrep"; break;
	case CMD_grepadd:   au_name = (char_u *)"grepadd"; break;
	case CMD_lgrepadd:  au_name = (char_u *)"lgrepadd"; break;
	default: break;
    }
    if (au_name != NULL && apply_autocmds(EVENT_QUICKFIXCMDPRE, au_name,
					       curbuf->b_fname, TRUE, curbuf))
    {
#ifdef FEAT_EVAL
	if (aborting())
	    return;
#endif
    }
#ifdef FEAT_MBYTE
    enc = (*curbuf->b_p_menc != NUL) ? curbuf->b_p_menc : p_menc;
#endif

    if (eap->cmdidx == CMD_lmake || eap->cmdidx == CMD_lgrep
	|| eap->cmdidx == CMD_lgrepadd)
	wp = curwin;

    autowrite_all();
    fname = get_mef_name();
    if (fname == NULL)
	return;
    mch_remove(fname);	    /* in case it's not unique */

    /*
     * If 'shellpipe' empty: don't redirect to 'errorfile'.
     */
    len = (unsigned)STRLEN(p_shq) * 2 + (unsigned)STRLEN(eap->arg) + 1;
    if (*p_sp != NUL)
	len += (unsigned)STRLEN(p_sp) + (unsigned)STRLEN(fname) + 3;
    cmd = alloc(len);
    if (cmd == NULL)
	return;
    sprintf((char *)cmd, "%s%s%s", (char *)p_shq, (char *)eap->arg,
							       (char *)p_shq);
    if (*p_sp != NUL)
	append_redir(cmd, len, p_sp, fname);
    /*
     * Output a newline if there's something else than the :make command that
     * was typed (in which case the cursor is in column 0).
     */
    if (msg_col == 0)
	msg_didout = FALSE;
    msg_start();
    MSG_PUTS(":!");
    msg_outtrans(cmd);		/* show what we are doing */

    /* let the shell know if we are redirecting output or not */
    do_shell(cmd, *p_sp != NUL ? SHELL_DOOUT : 0);

#ifdef AMIGA
    out_flush();
		/* read window status report and redraw before message */
    (void)char_avail();
#endif

    res = qf_init(wp, fname, (eap->cmdidx != CMD_make
			    && eap->cmdidx != CMD_lmake) ? p_gefm : p_efm,
					   (eap->cmdidx != CMD_grepadd
					    && eap->cmdidx != CMD_lgrepadd),
					   qf_cmdtitle(*eap->cmdlinep), enc);
    if (wp != NULL)
	qi = GET_LOC_LIST(wp);
    if (res >= 0 && qi != NULL)
	qf_list_changed(qi, qi->qf_curlist);
    if (au_name != NULL)
    {
	apply_autocmds(EVENT_QUICKFIXCMDPOST, au_name,
					       curbuf->b_fname, TRUE, curbuf);
	if (qi != NULL && qi->qf_curlist < qi->qf_listcount)
	    res = qi->qf_lists[qi->qf_curlist].qf_count;
	else
	    res = 0;
    }
    if (res > 0 && !eap->forceit)
	qf_jump(qi, 0, 0, FALSE);		/* display first error */

    mch_remove(fname);
    vim_free(fname);
    vim_free(cmd);
}

/*
 * Return the name for the errorfile, in allocated memory.
 * Find a new unique name when 'makeef' contains "##".
 * Returns NULL for error.
 */
    static char_u *
get_mef_name(void)
{
    char_u	*p;
    char_u	*name;
    static int	start = -1;
    static int	off = 0;
#ifdef HAVE_LSTAT
    stat_T	sb;
#endif

    if (*p_mef == NUL)
    {
	name = vim_tempname('e', FALSE);
	if (name == NULL)
	    EMSG(_(e_notmp));
	return name;
    }

    for (p = p_mef; *p; ++p)
	if (p[0] == '#' && p[1] == '#')
	    break;

    if (*p == NUL)
	return vim_strsave(p_mef);

    /* Keep trying until the name doesn't exist yet. */
    for (;;)
    {
	if (start == -1)
	    start = mch_get_pid();
	else
	    off += 19;

	name = alloc((unsigned)STRLEN(p_mef) + 30);
	if (name == NULL)
	    break;
	STRCPY(name, p_mef);
	sprintf((char *)name + (p - p_mef), "%d%d", start, off);
	STRCAT(name, p + 2);
	if (mch_getperm(name) < 0
#ifdef HAVE_LSTAT
		    /* Don't accept a symbolic link, it's a security risk. */
		    && mch_lstat((char *)name, &sb) < 0
#endif
		)
	    break;
	vim_free(name);
    }
    return name;
}

/*
 * Returns the number of valid entries in the current quickfix/location list.
 */
    int
qf_get_size(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    qfline_T	*qfp;
    int		i, sz = 0;
    int		prev_fnum = 0;

    if (eap->cmdidx == CMD_ldo || eap->cmdidx == CMD_lfdo)
    {
	/* Location list */
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	    return 0;
    }

    for (i = 0, qfp = qi->qf_lists[qi->qf_curlist].qf_start;
	    i < qi->qf_lists[qi->qf_curlist].qf_count && qfp != NULL;
	    ++i, qfp = qfp->qf_next)
    {
	if (qfp->qf_valid)
	{
	    if (eap->cmdidx == CMD_cdo || eap->cmdidx == CMD_ldo)
		sz++;	/* Count all valid entries */
	    else if (qfp->qf_fnum > 0 && qfp->qf_fnum != prev_fnum)
	    {
		/* Count the number of files */
		sz++;
		prev_fnum = qfp->qf_fnum;
	    }
	}
    }

    return sz;
}

/*
 * Returns the current index of the quickfix/location list.
 * Returns 0 if there is an error.
 */
    int
qf_get_cur_idx(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;

    if (eap->cmdidx == CMD_ldo || eap->cmdidx == CMD_lfdo)
    {
	/* Location list */
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	    return 0;
    }

    return qi->qf_lists[qi->qf_curlist].qf_index;
}

/*
 * Returns the current index in the quickfix/location list (counting only valid
 * entries). If no valid entries are in the list, then returns 1.
 */
    int
qf_get_cur_valid_idx(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    qf_list_T	*qfl;
    qfline_T	*qfp;
    int		i, eidx = 0;
    int		prev_fnum = 0;

    if (eap->cmdidx == CMD_ldo || eap->cmdidx == CMD_lfdo)
    {
	/* Location list */
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	    return 1;
    }

    qfl = &qi->qf_lists[qi->qf_curlist];
    qfp = qfl->qf_start;

    /* check if the list has valid errors */
    if (qfl->qf_count <= 0 || qfl->qf_nonevalid)
	return 1;

    for (i = 1; i <= qfl->qf_index && qfp!= NULL; i++, qfp = qfp->qf_next)
    {
	if (qfp->qf_valid)
	{
	    if (eap->cmdidx == CMD_cfdo || eap->cmdidx == CMD_lfdo)
	    {
		if (qfp->qf_fnum > 0 && qfp->qf_fnum != prev_fnum)
		{
		    /* Count the number of files */
		    eidx++;
		    prev_fnum = qfp->qf_fnum;
		}
	    }
	    else
		eidx++;
	}
    }

    return eidx ? eidx : 1;
}

/*
 * Get the 'n'th valid error entry in the quickfix or location list.
 * Used by :cdo, :ldo, :cfdo and :lfdo commands.
 * For :cdo and :ldo returns the 'n'th valid error entry.
 * For :cfdo and :lfdo returns the 'n'th valid file entry.
 */
    static int
qf_get_nth_valid_entry(qf_info_T *qi, int n, int fdo)
{
    qf_list_T	*qfl = &qi->qf_lists[qi->qf_curlist];
    qfline_T	*qfp = qfl->qf_start;
    int		i, eidx;
    int		prev_fnum = 0;

    /* check if the list has valid errors */
    if (qfl->qf_count <= 0 || qfl->qf_nonevalid)
	return 1;

    for (i = 1, eidx = 0; i <= qfl->qf_count && qfp != NULL;
	    i++, qfp = qfp->qf_next)
    {
	if (qfp->qf_valid)
	{
	    if (fdo)
	    {
		if (qfp->qf_fnum > 0 && qfp->qf_fnum != prev_fnum)
		{
		    /* Count the number of files */
		    eidx++;
		    prev_fnum = qfp->qf_fnum;
		}
	    }
	    else
		eidx++;
	}

	if (eidx == n)
	    break;
    }

    if (i <= qfl->qf_count)
	return i;
    else
	return 1;
}

/*
 * ":cc", ":crewind", ":cfirst" and ":clast".
 * ":ll", ":lrewind", ":lfirst" and ":llast".
 * ":cdo", ":ldo", ":cfdo" and ":lfdo"
 */
    void
ex_cc(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    int		errornr;

    if (eap->cmdidx == CMD_ll
	    || eap->cmdidx == CMD_lrewind
	    || eap->cmdidx == CMD_lfirst
	    || eap->cmdidx == CMD_llast
	    || eap->cmdidx == CMD_ldo
	    || eap->cmdidx == CMD_lfdo)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	{
	    EMSG(_(e_loclist));
	    return;
	}
    }

    if (eap->addr_count > 0)
	errornr = (int)eap->line2;
    else
    {
	if (eap->cmdidx == CMD_cc || eap->cmdidx == CMD_ll)
	    errornr = 0;
	else if (eap->cmdidx == CMD_crewind || eap->cmdidx == CMD_lrewind
		|| eap->cmdidx == CMD_cfirst || eap->cmdidx == CMD_lfirst)
	    errornr = 1;
	else
	    errornr = 32767;
    }

    /* For cdo and ldo commands, jump to the nth valid error.
     * For cfdo and lfdo commands, jump to the nth valid file entry.
     */
    if (eap->cmdidx == CMD_cdo || eap->cmdidx == CMD_ldo
	    || eap->cmdidx == CMD_cfdo || eap->cmdidx == CMD_lfdo)
	errornr = qf_get_nth_valid_entry(qi,
		eap->addr_count > 0 ? (int)eap->line1 : 1,
		eap->cmdidx == CMD_cfdo || eap->cmdidx == CMD_lfdo);

    qf_jump(qi, 0, errornr, eap->forceit);
}

/*
 * ":cnext", ":cnfile", ":cNext" and ":cprevious".
 * ":lnext", ":lNext", ":lprevious", ":lnfile", ":lNfile" and ":lpfile".
 * Also, used by ":cdo", ":ldo", ":cfdo" and ":lfdo" commands.
 */
    void
ex_cnext(exarg_T *eap)
{
    qf_info_T	*qi = &ql_info;
    int		errornr;

    if (eap->cmdidx == CMD_lnext
	    || eap->cmdidx == CMD_lNext
	    || eap->cmdidx == CMD_lprevious
	    || eap->cmdidx == CMD_lnfile
	    || eap->cmdidx == CMD_lNfile
	    || eap->cmdidx == CMD_lpfile
	    || eap->cmdidx == CMD_ldo
	    || eap->cmdidx == CMD_lfdo)
    {
	qi = GET_LOC_LIST(curwin);
	if (qi == NULL)
	{
	    EMSG(_(e_loclist));
	    return;
	}
    }

    if (eap->addr_count > 0
	    && (eap->cmdidx != CMD_cdo && eap->cmdidx != CMD_ldo
		&& eap->cmdidx != CMD_cfdo && eap->cmdidx != CMD_lfdo))
	errornr = (int)eap->line2;
    else
	errornr = 1;

    qf_jump(qi, (eap->cmdidx == CMD_cnext || eap->cmdidx == CMD_lnext
		|| eap->cmdidx == CMD_cdo || eap->cmdidx == CMD_ldo)
	    ? FORWARD
	    : (eap->cmdidx == CMD_cnfile || eap->cmdidx == CMD_lnfile
		|| eap->cmdidx == CMD_cfdo || eap->cmdidx == CMD_lfdo)
		? FORWARD_FILE
		: (eap->cmdidx == CMD_cpfile || eap->cmdidx == CMD_lpfile
		   || eap->cmdidx == CMD_cNfile || eap->cmdidx == CMD_lNfile)
		    ? BACKWARD_FILE
		    : BACKWARD,
	    errornr, eap->forceit);
}

/*
 * ":cfile"/":cgetfile"/":caddfile" commands.
 * ":lfile"/":lgetfile"/":laddfile" commands.
 */
    void
ex_cfile(exarg_T *eap)
{
    char_u	*enc = NULL;
    win_T	*wp = NULL;
    qf_info_T	*qi = &ql_info;
    char_u	*au_name = NULL;
    int		save_qfid = 0;  /* init for gcc */
    int		res;

    switch (eap->cmdidx)
    {
	case CMD_cfile:	    au_name = (char_u *)"cfile"; break;
	case CMD_cgetfile:  au_name = (char_u *)"cgetfile"; break;
	case CMD_caddfile:  au_name = (char_u *)"caddfile"; break;
	case CMD_lfile:	    au_name = (char_u *)"lfile"; break;
	case CMD_lgetfile:  au_name = (char_u *)"lgetfile"; break;
	case CMD_laddfile:  au_name = (char_u *)"laddfile"; break;
	default: break;
    }
    if (au_name != NULL)
	apply_autocmds(EVENT_QUICKFIXCMDPRE, au_name, NULL, FALSE, curbuf);
#ifdef FEAT_MBYTE
    enc = (*curbuf->b_p_menc != NUL) ? curbuf->b_p_menc : p_menc;
#endif
#ifdef FEAT_BROWSE
    if (cmdmod.browse)
    {
	char_u *browse_file = do_browse(0, (char_u *)_("Error file"), eap->arg,
				   NULL, NULL,
				   (char_u *)_(BROWSE_FILTER_ALL_FILES), NULL);
	if (browse_file == NULL)
	    return;
	set_string_option_direct((char_u *)"ef", -1, browse_file, OPT_FREE, 0);
	vim_free(browse_file);
    }
    else
#endif
    if (*eap->arg != NUL)
	set_string_option_direct((char_u *)"ef", -1, eap->arg, OPT_FREE, 0);

    if (eap->cmdidx == CMD_lfile
	    || eap->cmdidx == CMD_lgetfile
	    || eap->cmdidx == CMD_laddfile)
	wp = curwin;

    /*
     * This function is used by the :cfile, :cgetfile and :caddfile
     * commands.
     * :cfile always creates a new quickfix list and jumps to the
     * first error.
     * :cgetfile creates a new quickfix list but doesn't jump to the
     * first error.
     * :caddfile adds to an existing quickfix list. If there is no
     * quickfix list then a new list is created.
     */
    res = qf_init(wp, p_ef, p_efm, (eap->cmdidx != CMD_caddfile
			&& eap->cmdidx != CMD_laddfile),
			qf_cmdtitle(*eap->cmdlinep), enc);
    if (wp != NULL)
	qi = GET_LOC_LIST(wp);
    if (res >= 0 && qi != NULL)
	qf_list_changed(qi, qi->qf_curlist);
    if (qi != NULL)
	save_qfid = qi->qf_lists[qi->qf_curlist].qf_id;
    if (au_name != NULL)
	apply_autocmds(EVENT_QUICKFIXCMDPOST, au_name, NULL, FALSE, curbuf);

    /* An autocmd might have freed the quickfix/location list. Check whether it
     * is still valid. */
    if (qi != NULL && !qflist_valid(wp, save_qfid))
	return;
    if (res > 0 && (eap->cmdidx == CMD_cfile || eap->cmdidx == CMD_lfile))
	qf_jump(qi, 0, 0, eap->forceit);	/* display first error */
}

/*
 * Return the quickfix/location list number with the given identifier.
 * Returns -1 if list is not found.
 */
    static int
qf_id2nr(qf_info_T *qi, int_u qfid)
{
    int		qf_idx;

    for (qf_idx = 0; qf_idx < qi->qf_listcount; qf_idx++)
	if (qi->qf_lists[qf_idx].qf_id == qfid)
	    return qf_idx;
    return INVALID_QFIDX;
}

/*
 * Return the vimgrep autocmd name.
 */
    static char_u *
vgr_get_auname(cmdidx_T cmdidx)
{
    switch (cmdidx)
    {
	case CMD_vimgrep:     return (char_u *)"vimgrep";
	case CMD_lvimgrep:    return (char_u *)"lvimgrep";
	case CMD_vimgrepadd:  return (char_u *)"vimgrepadd";
	case CMD_lvimgrepadd: return (char_u *)"lvimgrepadd";
	case CMD_grep:	      return (char_u *)"grep";
	case CMD_lgrep:	      return (char_u *)"lgrep";
	case CMD_grepadd:     return (char_u *)"grepadd";
	case CMD_lgrepadd:    return (char_u *)"lgrepadd";
	default: return NULL;
    }
}

/*
 * Initialize the regmatch used by vimgrep for pattern "s".
 */
    static void
vgr_init_regmatch(regmmatch_T *regmatch, char_u *s)
{
    /* Get the search pattern: either white-separated or enclosed in // */
    regmatch->regprog = NULL;

    if (s == NULL || *s == NUL)
    {
	/* Pattern is empty, use last search pattern. */
	if (last_search_pat() == NULL)
	{
	    EMSG(_(e_noprevre));
	    return;
	}
	regmatch->regprog = vim_regcomp(last_search_pat(), RE_MAGIC);
    }
    else
	regmatch->regprog = vim_regcomp(s, RE_MAGIC);

    regmatch->rmm_ic = p_ic;
    regmatch->rmm_maxcol = 0;
}

/*
 * Display a file name when vimgrep is running.
 */
    static void
vgr_display_fname(char_u *fname)
{
    char_u	*p;

    msg_start();
    p = msg_strtrunc(fname, TRUE);
    if (p == NULL)
	msg_outtrans(fname);
    else
    {
	msg_outtrans(p);
	vim_free(p);
    }
    msg_clr_eos();
    msg_didout = FALSE;	    /* overwrite this message */
    msg_nowait = TRUE;	    /* don't wait for this message */
    msg_col = 0;
    out_flush();
}

/*
 * Load a dummy buffer to search for a pattern using vimgrep.
 */
    static buf_T *
vgr_load_dummy_buf(
	char_u *fname,
	char_u *dirname_start,
	char_u *dirname_now)
{
    int		save_mls;
#if defined(FEAT_SYN_HL)
    char_u	*save_ei = NULL;
#endif
    buf_T	*buf;

#if defined(FEAT_SYN_HL)
    /* Don't do Filetype autocommands to avoid loading syntax and
     * indent scripts, a great speed improvement. */
    save_ei = au_event_disable(",Filetype");
#endif
    /* Don't use modelines here, it's useless. */
    save_mls = p_mls;
    p_mls = 0;

    /* Load file into a buffer, so that 'fileencoding' is detected,
     * autocommands applied, etc. */
    buf = load_dummy_buffer(fname, dirname_start, dirname_now);

    p_mls = save_mls;
#if defined(FEAT_SYN_HL)
    au_event_restore(save_ei);
#endif

    return buf;
}

/*
 * Check whether a quickfix/location list valid. Autocmds may remove or change
 * a quickfix list when vimgrep is running. If the list is not found, create a
 * new list.
 */
    static int
vgr_qflist_valid(
	win_T	    *wp,
	qf_info_T   *qi,
	int_u	    qfid,
	char_u	    *title)
{
    /* Verify that the quickfix/location list was not freed by an autocmd */
    if (!qflist_valid(wp, qfid))
    {
	if (wp != NULL)
	{
	    /* An autocmd has freed the location list. */
	    EMSG(_(e_loc_list_changed));
	    return FALSE;
	}
	else
	{
	    /* Quickfix list is not found, create a new one. */
	    qf_new_list(qi, title);
	    return TRUE;
	}
    }

    if (qi->qf_lists[qi->qf_curlist].qf_id != qfid)
	/* Autocommands changed the quickfix list.  Find the one we were
	 * using and restore it. */
	qi->qf_curlist = qf_id2nr(qi, qfid);

    return TRUE;
}

/*
 * Search for a pattern in all the lines in a buffer and add the matching lines
 * to a quickfix list.
 */
    static int
vgr_match_buflines(
	qf_info_T   *qi,
	char_u	    *fname,
	buf_T	    *buf,
	regmmatch_T *regmatch,
	long	    tomatch,
	int	    duplicate_name,
	int	    flags)
{
    int		found_match = FALSE;
    long	lnum;
    colnr_T	col;

    for (lnum = 1; lnum <= buf->b_ml.ml_line_count && tomatch > 0; ++lnum)
    {
	col = 0;
	while (vim_regexec_multi(regmatch, curwin, buf, lnum,
		    col, NULL, NULL) > 0)
	{
	    /* Pass the buffer number so that it gets used even for a
	     * dummy buffer, unless duplicate_name is set, then the
	     * buffer will be wiped out below. */
	    if (qf_add_entry(qi,
			qi->qf_curlist,
			NULL,       /* dir */
			fname,
			NULL,
			duplicate_name ? 0 : buf->b_fnum,
			ml_get_buf(buf,
			    regmatch->startpos[0].lnum + lnum, FALSE),
			regmatch->startpos[0].lnum + lnum,
			regmatch->startpos[0].col + 1,
			FALSE,      /* vis_col */
			NULL,	    /* search pattern */
			0,	    /* nr */
			0,	    /* type */
			TRUE	    /* valid */
			) == FAIL)
	    {
		got_int = TRUE;
		break;
	    }
	    found_match = TRUE;
	    if (--tomatch == 0)
		break;
	    if ((flags & VGR_GLOBAL) == 0
		    || regmatch->endpos[0].lnum > 0)
		break;
	    col = regmatch->endpos[0].col
		+ (col == regmatch->endpos[0].col);
	    if (col > (colnr_T)STRLEN(ml_get_buf(buf, lnum, FALSE)))
		break;
	}
	line_breakcheck();
	if (got_int)
	    break;
    }

    return found_match;
}

/*
 * Jump to the first match and update the directory.
 */
    static void
vgr_jump_to_match(
	qf_info_T   *qi,
	int	    forceit,
	int	    *redraw_for_dummy,
	buf_T	    *first_match_buf,
	char_u	    *target_dir)
{
    buf_T	*buf;

    buf = curbuf;
    qf_jump(qi, 0, 0, forceit);
    if (buf != curbuf)
	/* If we jumped to another buffer redrawing will already be
	 * taken care of. */
	*redraw_for_dummy = FALSE;

    /* Jump to the directory used after loading the buffer. */
    if (curbuf == first_match_buf && target_dir != NULL)
    {
	exarg_T ea;

	ea.arg = target_dir;
	ea.cmdidx = CMD_lcd;
	ex_cd(&ea);
    }
}

/*
 * ":vimgrep {pattern} file(s)"
 * ":vimgrepadd {pattern} file(s)"
 * ":lvimgrep {pattern} file(s)"
 * ":lvimgrepadd {pattern} file(s)"
 */
    void
ex_vimgrep(exarg_T *eap)
{
    regmmatch_T	regmatch;
    int		fcount;
    char_u	**fnames;
    char_u	*fname;
    char_u	*title;
    char_u	*s;
    char_u	*p;
    int		fi;
    qf_info_T	*qi = &ql_info;
    int_u	save_qfid;
    win_T	*wp = NULL;
    buf_T	*buf;
    int		duplicate_name = FALSE;
    int		using_dummy;
    int		redraw_for_dummy = FALSE;
    int		found_match;
    buf_T	*first_match_buf = NULL;
    time_t	seconds = 0;
    aco_save_T	aco;
    int		flags = 0;
    long	tomatch;
    char_u	*dirname_start = NULL;
    char_u	*dirname_now = NULL;
    char_u	*target_dir = NULL;
    char_u	*au_name =  NULL;

    au_name = vgr_get_auname(eap->cmdidx);
    if (au_name != NULL && apply_autocmds(EVENT_QUICKFIXCMDPRE, au_name,
					       curbuf->b_fname, TRUE, curbuf))
    {
#ifdef FEAT_EVAL
	if (aborting())
	    return;
#endif
    }

    if (eap->cmdidx == CMD_lgrep
	    || eap->cmdidx == CMD_lvimgrep
	    || eap->cmdidx == CMD_lgrepadd
	    || eap->cmdidx == CMD_lvimgrepadd)
    {
	qi = ll_get_or_alloc_list(curwin);
	if (qi == NULL)
	    return;
	wp = curwin;
    }

    if (eap->addr_count > 0)
	tomatch = eap->line2;
    else
	tomatch = MAXLNUM;

    /* Get the search pattern: either white-separated or enclosed in // */
    regmatch.regprog = NULL;
    title = vim_strsave(qf_cmdtitle(*eap->cmdlinep));
    p = skip_vimgrep_pat(eap->arg, &s, &flags);
    if (p == NULL)
    {
	EMSG(_(e_invalpat));
	goto theend;
    }

    vgr_init_regmatch(&regmatch, s);
    if (regmatch.regprog == NULL)
	goto theend;

    p = skipwhite(p);
    if (*p == NUL)
    {
	EMSG(_("E683: File name missing or invalid pattern"));
	goto theend;
    }

    if ((eap->cmdidx != CMD_grepadd && eap->cmdidx != CMD_lgrepadd
		&& eap->cmdidx != CMD_vimgrepadd
		&& eap->cmdidx != CMD_lvimgrepadd)
					|| qi->qf_curlist == qi->qf_listcount)
	/* make place for a new list */
	qf_new_list(qi, title != NULL ? title : qf_cmdtitle(*eap->cmdlinep));

    /* parse the list of arguments */
    if (get_arglist_exp(p, &fcount, &fnames, TRUE) == FAIL)
	goto theend;
    if (fcount == 0)
    {
	EMSG(_(e_nomatch));
	goto theend;
    }

    dirname_start = alloc_id(MAXPATHL, aid_qf_dirname_start);
    dirname_now = alloc_id(MAXPATHL, aid_qf_dirname_now);
    if (dirname_start == NULL || dirname_now == NULL)
    {
	FreeWild(fcount, fnames);
	goto theend;
    }

    /* Remember the current directory, because a BufRead autocommand that does
     * ":lcd %:p:h" changes the meaning of short path names. */
    mch_dirname(dirname_start, MAXPATHL);

     /* Remember the current quickfix list identifier, so that we can check for
      * autocommands changing the current quickfix list. */
    save_qfid = qi->qf_lists[qi->qf_curlist].qf_id;

    seconds = (time_t)0;
    for (fi = 0; fi < fcount && !got_int && tomatch > 0; ++fi)
    {
	fname = shorten_fname1(fnames[fi]);
	if (time(NULL) > seconds)
	{
	    /* Display the file name every second or so, show the user we are
	     * working on it. */
	    seconds = time(NULL);
	    vgr_display_fname(fname);
	}

	buf = buflist_findname_exp(fnames[fi]);
	if (buf == NULL || buf->b_ml.ml_mfp == NULL)
	{
	    /* Remember that a buffer with this name already exists. */
	    duplicate_name = (buf != NULL);
	    using_dummy = TRUE;
	    redraw_for_dummy = TRUE;

	    buf = vgr_load_dummy_buf(fname, dirname_start, dirname_now);
	}
	else
	    /* Use existing, loaded buffer. */
	    using_dummy = FALSE;

	/* Check whether the quickfix list is still valid. When loading a
	 * buffer above, autocommands might have changed the quickfix list. */
	if (!vgr_qflist_valid(wp, qi, save_qfid, qf_cmdtitle(*eap->cmdlinep)))
	{
	    FreeWild(fcount, fnames);
	    goto theend;
	}
	save_qfid = qi->qf_lists[qi->qf_curlist].qf_id;

	if (buf == NULL)
	{
	    if (!got_int)
		smsg((char_u *)_("Cannot open file \"%s\""), fname);
	}
	else
	{
	    /* Try for a match in all lines of the buffer.
	     * For ":1vimgrep" look for first match only. */
	    found_match = vgr_match_buflines(qi, fname, buf, &regmatch,
		    tomatch, duplicate_name, flags);

	    if (using_dummy)
	    {
		if (found_match && first_match_buf == NULL)
		    first_match_buf = buf;
		if (duplicate_name)
		{
		    /* Never keep a dummy buffer if there is another buffer
		     * with the same name. */
		    wipe_dummy_buffer(buf, dirname_start);
		    buf = NULL;
		}
		else if (!cmdmod.hide
			    || buf->b_p_bh[0] == 'u'	/* "unload" */
			    || buf->b_p_bh[0] == 'w'	/* "wipe" */
			    || buf->b_p_bh[0] == 'd')	/* "delete" */
		{
		    /* When no match was found we don't need to remember the
		     * buffer, wipe it out.  If there was a match and it
		     * wasn't the first one or we won't jump there: only
		     * unload the buffer.
		     * Ignore 'hidden' here, because it may lead to having too
		     * many swap files. */
		    if (!found_match)
		    {
			wipe_dummy_buffer(buf, dirname_start);
			buf = NULL;
		    }
		    else if (buf != first_match_buf || (flags & VGR_NOJUMP))
		    {
			unload_dummy_buffer(buf, dirname_start);
			/* Keeping the buffer, remove the dummy flag. */
			buf->b_flags &= ~BF_DUMMY;
			buf = NULL;
		    }
		}

		if (buf != NULL)
		{
		    /* Keeping the buffer, remove the dummy flag. */
		    buf->b_flags &= ~BF_DUMMY;

		    /* If the buffer is still loaded we need to use the
		     * directory we jumped to below. */
		    if (buf == first_match_buf
			    && target_dir == NULL
			    && STRCMP(dirname_start, dirname_now) != 0)
			target_dir = vim_strsave(dirname_now);

		    /* The buffer is still loaded, the Filetype autocommands
		     * need to be done now, in that buffer.  And the modelines
		     * need to be done (again).  But not the window-local
		     * options! */
		    aucmd_prepbuf(&aco, buf);
#if defined(FEAT_SYN_HL)
		    apply_autocmds(EVENT_FILETYPE, buf->b_p_ft,
						     buf->b_fname, TRUE, buf);
#endif
		    do_modelines(OPT_NOWIN);
		    aucmd_restbuf(&aco);
		}
	    }
	}
    }

    FreeWild(fcount, fnames);

    qi->qf_lists[qi->qf_curlist].qf_nonevalid = FALSE;
    qi->qf_lists[qi->qf_curlist].qf_ptr = qi->qf_lists[qi->qf_curlist].qf_start;
    qi->qf_lists[qi->qf_curlist].qf_index = 1;
    qf_list_changed(qi, qi->qf_curlist);

    qf_update_buffer(qi, NULL);

    if (au_name != NULL)
	apply_autocmds(EVENT_QUICKFIXCMDPOST, au_name,
					       curbuf->b_fname, TRUE, curbuf);
    /*
     * The QuickFixCmdPost autocmd may free the quickfix list. Check the list
     * is still valid.
     */
    if (!qflist_valid(wp, save_qfid))
	goto theend;

    /* Jump to first match. */
    if (qi->qf_lists[qi->qf_curlist].qf_count > 0)
    {
	if ((flags & VGR_NOJUMP) == 0)
	    vgr_jump_to_match(qi, eap->forceit, &redraw_for_dummy,
		    first_match_buf, target_dir);
    }
    else
	EMSG2(_(e_nomatch2), s);

    /* If we loaded a dummy buffer into the current window, the autocommands
     * may have messed up things, need to redraw and recompute folds. */
    if (redraw_for_dummy)
    {
#ifdef FEAT_FOLDING
	foldUpdateAll(curwin);
#else
	redraw_later(NOT_VALID);
#endif
    }

theend:
    vim_free(title);
    vim_free(dirname_now);
    vim_free(dirname_start);
    vim_free(target_dir);
    vim_regfree(regmatch.regprog);
}

/*
 * Restore current working directory to "dirname_start" if they differ, taking
 * into account whether it is set locally or globally.
 */
    static void
restore_start_dir(char_u *dirname_start)
{
    char_u *dirname_now = alloc(MAXPATHL);

    if (NULL != dirname_now)
    {
	mch_dirname(dirname_now, MAXPATHL);
	if (STRCMP(dirname_start, dirname_now) != 0)
	{
	    /* If the directory has changed, change it back by building up an
	     * appropriate ex command and executing it. */
	    exarg_T ea;

	    ea.arg = dirname_start;
	    ea.cmdidx = (curwin->w_localdir == NULL) ? CMD_cd : CMD_lcd;
	    ex_cd(&ea);
	}
	vim_free(dirname_now);
    }
}

/*
 * Load file "fname" into a dummy buffer and return the buffer pointer,
 * placing the directory resulting from the buffer load into the
 * "resulting_dir" pointer. "resulting_dir" must be allocated by the caller
 * prior to calling this function. Restores directory to "dirname_start" prior
 * to returning, if autocmds or the 'autochdir' option have changed it.
 *
 * If creating the dummy buffer does not fail, must call unload_dummy_buffer()
 * or wipe_dummy_buffer() later!
 *
 * Returns NULL if it fails.
 */
    static buf_T *
load_dummy_buffer(
    char_u	*fname,
    char_u	*dirname_start,  /* in: old directory */
    char_u	*resulting_dir)  /* out: new directory */
{
    buf_T	*newbuf;
    bufref_T	newbufref;
    bufref_T	newbuf_to_wipe;
    int		failed = TRUE;
    aco_save_T	aco;
    int		readfile_result;

    /* Allocate a buffer without putting it in the buffer list. */
    newbuf = buflist_new(NULL, NULL, (linenr_T)1, BLN_DUMMY);
    if (newbuf == NULL)
	return NULL;
    set_bufref(&newbufref, newbuf);

    /* Init the options. */
    buf_copy_options(newbuf, BCO_ENTER | BCO_NOHELP);

    /* need to open the memfile before putting the buffer in a window */
    if (ml_open(newbuf) == OK)
    {
	/* Make sure this buffer isn't wiped out by auto commands. */
	++newbuf->b_locked;

	/* set curwin/curbuf to buf and save a few things */
	aucmd_prepbuf(&aco, newbuf);

	/* Need to set the filename for autocommands. */
	(void)setfname(curbuf, fname, NULL, FALSE);

	/* Create swap file now to avoid the ATTENTION message. */
	check_need_swap(TRUE);

	/* Remove the "dummy" flag, otherwise autocommands may not
	 * work. */
	curbuf->b_flags &= ~BF_DUMMY;

	newbuf_to_wipe.br_buf = NULL;
	readfile_result = readfile(fname, NULL,
		    (linenr_T)0, (linenr_T)0, (linenr_T)MAXLNUM,
		    NULL, READ_NEW | READ_DUMMY);
	--newbuf->b_locked;
	if (readfile_result == OK
		&& !got_int
		&& !(curbuf->b_flags & BF_NEW))
	{
	    failed = FALSE;
	    if (curbuf != newbuf)
	    {
		/* Bloody autocommands changed the buffer!  Can happen when
		 * using netrw and editing a remote file.  Use the current
		 * buffer instead, delete the dummy one after restoring the
		 * window stuff. */
		set_bufref(&newbuf_to_wipe, newbuf);
		newbuf = curbuf;
	    }
	}

	/* restore curwin/curbuf and a few other things */
	aucmd_restbuf(&aco);
	if (newbuf_to_wipe.br_buf != NULL && bufref_valid(&newbuf_to_wipe))
	    wipe_buffer(newbuf_to_wipe.br_buf, FALSE);

	/* Add back the "dummy" flag, otherwise buflist_findname_stat() won't
	 * skip it. */
	newbuf->b_flags |= BF_DUMMY;
    }

    /*
     * When autocommands/'autochdir' option changed directory: go back.
     * Let the caller know what the resulting dir was first, in case it is
     * important.
     */
    mch_dirname(resulting_dir, MAXPATHL);
    restore_start_dir(dirname_start);

    if (!bufref_valid(&newbufref))
	return NULL;
    if (failed)
    {
	wipe_dummy_buffer(newbuf, dirname_start);
	return NULL;
    }
    return newbuf;
}

/*
 * Wipe out the dummy buffer that load_dummy_buffer() created. Restores
 * directory to "dirname_start" prior to returning, if autocmds or the
 * 'autochdir' option have changed it.
 */
    static void
wipe_dummy_buffer(buf_T *buf, char_u *dirname_start)
{
    if (curbuf != buf)		/* safety check */
    {
#if defined(FEAT_EVAL)
	cleanup_T   cs;

	/* Reset the error/interrupt/exception state here so that aborting()
	 * returns FALSE when wiping out the buffer.  Otherwise it doesn't
	 * work when got_int is set. */
	enter_cleanup(&cs);
#endif

	wipe_buffer(buf, FALSE);

#if defined(FEAT_EVAL)
	/* Restore the error/interrupt/exception state if not discarded by a
	 * new aborting error, interrupt, or uncaught exception. */
	leave_cleanup(&cs);
#endif
	/* When autocommands/'autochdir' option changed directory: go back. */
	restore_start_dir(dirname_start);
    }
}

/*
 * Unload the dummy buffer that load_dummy_buffer() created. Restores
 * directory to "dirname_start" prior to returning, if autocmds or the
 * 'autochdir' option have changed it.
 */
    static void
unload_dummy_buffer(buf_T *buf, char_u *dirname_start)
{
    if (curbuf != buf)		/* safety check */
    {
	close_buffer(NULL, buf, DOBUF_UNLOAD, FALSE);

	/* When autocommands/'autochdir' option changed directory: go back. */
	restore_start_dir(dirname_start);
    }
}

#if defined(FEAT_EVAL) || defined(PROTO)
/*
 * Add each quickfix error to list "list" as a dictionary.
 * If qf_idx is -1, use the current list. Otherwise, use the specified list.
 */
    int
get_errorlist(qf_info_T *qi_arg, win_T *wp, int qf_idx, list_T *list)
{
    qf_info_T	*qi = qi_arg;
    dict_T	*dict;
    char_u	buf[2];
    qfline_T	*qfp;
    int		i;
    int		bufnum;

    if (qi == NULL)
    {
	qi = &ql_info;
	if (wp != NULL)
	{
	    qi = GET_LOC_LIST(wp);
	    if (qi == NULL)
		return FAIL;
	}
    }

    if (qf_idx == INVALID_QFIDX)
	qf_idx = qi->qf_curlist;

    if (qf_idx >= qi->qf_listcount
	    || qi->qf_lists[qf_idx].qf_count == 0)
	return FAIL;

    qfp = qi->qf_lists[qf_idx].qf_start;
    for (i = 1; !got_int && i <= qi->qf_lists[qf_idx].qf_count; ++i)
    {
	/* Handle entries with a non-existing buffer number. */
	bufnum = qfp->qf_fnum;
	if (bufnum != 0 && (buflist_findnr(bufnum) == NULL))
	    bufnum = 0;

	if ((dict = dict_alloc()) == NULL)
	    return FAIL;
	if (list_append_dict(list, dict) == FAIL)
	    return FAIL;

	buf[0] = qfp->qf_type;
	buf[1] = NUL;
	if ( dict_add_nr_str(dict, "bufnr", (long)bufnum, NULL) == FAIL
	  || dict_add_nr_str(dict, "lnum",  (long)qfp->qf_lnum, NULL) == FAIL
	  || dict_add_nr_str(dict, "col",   (long)qfp->qf_col, NULL) == FAIL
	  || dict_add_nr_str(dict, "vcol",  (long)qfp->qf_viscol, NULL) == FAIL
	  || dict_add_nr_str(dict, "nr",    (long)qfp->qf_nr, NULL) == FAIL
	  || dict_add_nr_str(dict, "module",  0L,
		   qfp->qf_module == NULL ? (char_u *)"" : qfp->qf_module) == FAIL
	  || dict_add_nr_str(dict, "pattern",  0L,
	     qfp->qf_pattern == NULL ? (char_u *)"" : qfp->qf_pattern) == FAIL
	  || dict_add_nr_str(dict, "text",  0L,
		   qfp->qf_text == NULL ? (char_u *)"" : qfp->qf_text) == FAIL
	  || dict_add_nr_str(dict, "type",  0L, buf) == FAIL
	  || dict_add_nr_str(dict, "valid", (long)qfp->qf_valid, NULL) == FAIL)
	    return FAIL;

	qfp = qfp->qf_next;
	if (qfp == NULL)
	    break;
    }
    return OK;
}

/*
 * Flags used by getqflist()/getloclist() to determine which fields to return.
 */
enum {
    QF_GETLIST_NONE	= 0x0,
    QF_GETLIST_TITLE	= 0x1,
    QF_GETLIST_ITEMS	= 0x2,
    QF_GETLIST_NR	= 0x4,
    QF_GETLIST_WINID	= 0x8,
    QF_GETLIST_CONTEXT	= 0x10,
    QF_GETLIST_ID	= 0x20,
    QF_GETLIST_IDX	= 0x40,
    QF_GETLIST_SIZE	= 0x80,
    QF_GETLIST_TICK	= 0x100,
    QF_GETLIST_ALL	= 0x1FF,
};

/*
 * Parse text from 'di' and return the quickfix list items.
 * Existing quickfix lists are not modified.
 */
    static int
qf_get_list_from_lines(dict_T *what, dictitem_T *di, dict_T *retdict)
{
    int		status = FAIL;
    qf_info_T	*qi;
    char_u	*errorformat = p_efm;
    dictitem_T	*efm_di;
    list_T	*l;

    /* Only a List value is supported */
    if (di->di_tv.v_type == VAR_LIST && di->di_tv.vval.v_list != NULL)
    {
	/* If errorformat is supplied then use it, otherwise use the 'efm'
	 * option setting
	 */
	if ((efm_di = dict_find(what, (char_u *)"efm", -1)) != NULL)
	{
	    if (efm_di->di_tv.v_type != VAR_STRING ||
		    efm_di->di_tv.vval.v_string == NULL)
		return FAIL;
	    errorformat = efm_di->di_tv.vval.v_string;
	}

	l = list_alloc();
	if (l == NULL)
	    return FAIL;

	qi = (qf_info_T *)alloc((unsigned)sizeof(qf_info_T));
	if (qi != NULL)
	{
	    vim_memset(qi, 0, (size_t)(sizeof(qf_info_T)));
	    qi->qf_refcount++;

	    if (qf_init_ext(qi, 0, NULL, NULL, &di->di_tv, errorformat,
			TRUE, (linenr_T)0, (linenr_T)0, NULL, NULL) > 0)
	    {
		(void)get_errorlist(qi, NULL, 0, l);
		qf_free(qi, 0);
	    }
	    free(qi);
	}
	dict_add_list(retdict, "items", l);
	status = OK;
    }

    return status;
}

/*
 * Return the quickfix/location list window identifier in the current tabpage.
 */
    static int
qf_winid(qf_info_T *qi)
{
    win_T	*win;

    /* The quickfix window can be opened even if the quickfix list is not set
     * using ":copen". This is not true for location lists.  */
    if (qi == NULL)
	return 0;
    win = qf_find_win(qi);
    if (win != NULL)
	return win->w_id;
    return 0;
}

/*
 * Convert the keys in 'what' to quickfix list property flags.
 */
    static int
qf_getprop_keys2flags(dict_T *what)
{
    int		flags = QF_GETLIST_NONE;

    if (dict_find(what, (char_u *)"all", -1) != NULL)
	flags |= QF_GETLIST_ALL;

    if (dict_find(what, (char_u *)"title", -1) != NULL)
	flags |= QF_GETLIST_TITLE;

    if (dict_find(what, (char_u *)"nr", -1) != NULL)
	flags |= QF_GETLIST_NR;

    if (dict_find(what, (char_u *)"winid", -1) != NULL)
	flags |= QF_GETLIST_WINID;

    if (dict_find(what, (char_u *)"context", -1) != NULL)
	flags |= QF_GETLIST_CONTEXT;

    if (dict_find(what, (char_u *)"id", -1) != NULL)
	flags |= QF_GETLIST_ID;

    if (dict_find(what, (char_u *)"items", -1) != NULL)
	flags |= QF_GETLIST_ITEMS;

    if (dict_find(what, (char_u *)"idx", -1) != NULL)
	flags |= QF_GETLIST_IDX;

    if (dict_find(what, (char_u *)"size", -1) != NULL)
	flags |= QF_GETLIST_SIZE;

    if (dict_find(what, (char_u *)"changedtick", -1) != NULL)
	flags |= QF_GETLIST_TICK;

    return flags;
}

/*
 * Return the quickfix list index based on 'nr' or 'id' in 'what'.
 * If 'nr' and 'id' are not present in 'what' then return the current
 * quickfix list index.
 * If 'nr' is zero then return the current quickfix list index.
 * If 'nr' is '$' then return the last quickfix list index.
 * If 'id' is present then return the index of the quickfix list with that id.
 * If 'id' is zero then return the quickfix list index specified by 'nr'.
 * Return -1, if quickfix list is not present or if the stack is empty.
 */
    static int
qf_getprop_qfidx(qf_info_T *qi, dict_T *what)
{
    int		qf_idx;
    dictitem_T	*di;

    qf_idx = qi->qf_curlist;	/* default is the current list */
    if ((di = dict_find(what, (char_u *)"nr", -1)) != NULL)
    {
	/* Use the specified quickfix/location list */
	if (di->di_tv.v_type == VAR_NUMBER)
	{
	    /* for zero use the current list */
	    if (di->di_tv.vval.v_number != 0)
	    {
		qf_idx = di->di_tv.vval.v_number - 1;
		if (qf_idx < 0 || qf_idx >= qi->qf_listcount)
		    qf_idx = INVALID_QFIDX;
	    }
	}
	else if (di->di_tv.v_type == VAR_STRING
		&& di->di_tv.vval.v_string != NULL
		&& STRCMP(di->di_tv.vval.v_string, "$") == 0)
	    /* Get the last quickfix list number */
	    qf_idx = qi->qf_listcount - 1;
	else
	    qf_idx = INVALID_QFIDX;
    }

    if ((di = dict_find(what, (char_u *)"id", -1)) != NULL)
    {
	/* Look for a list with the specified id */
	if (di->di_tv.v_type == VAR_NUMBER)
	{
	    /*
	     * For zero, use the current list or the list specified by 'nr'
	     */
	    if (di->di_tv.vval.v_number != 0)
		qf_idx = qf_id2nr(qi, di->di_tv.vval.v_number);
	}
	else
	    qf_idx = INVALID_QFIDX;
    }

    return qf_idx;
}

/*
 * Return default values for quickfix list properties in retdict.
 */
    static int
qf_getprop_defaults(qf_info_T *qi, int flags, dict_T *retdict)
{
    int		status = OK;

    if (flags & QF_GETLIST_TITLE)
	status = dict_add_nr_str(retdict, "title", 0L, (char_u *)"");
    if ((status == OK) && (flags & QF_GETLIST_ITEMS))
    {
	list_T	*l = list_alloc();
	if (l != NULL)
	    status = dict_add_list(retdict, "items", l);
	else
	    status = FAIL;
    }
    if ((status == OK) && (flags & QF_GETLIST_NR))
	status = dict_add_nr_str(retdict, "nr", 0L, NULL);
    if ((status == OK) && (flags & QF_GETLIST_WINID))
	status = dict_add_nr_str(retdict, "winid", qf_winid(qi), NULL);
    if ((status == OK) && (flags & QF_GETLIST_CONTEXT))
	status = dict_add_nr_str(retdict, "context", 0L, (char_u *)"");
    if ((status == OK) && (flags & QF_GETLIST_ID))
	status = dict_add_nr_str(retdict, "id", 0L, NULL);
    if ((status == OK) && (flags & QF_GETLIST_IDX))
	status = dict_add_nr_str(retdict, "idx", 0L, NULL);
    if ((status == OK) && (flags & QF_GETLIST_SIZE))
	status = dict_add_nr_str(retdict, "size", 0L, NULL);
    if ((status == OK) && (flags & QF_GETLIST_TICK))
	status = dict_add_nr_str(retdict, "changedtick", 0L, NULL);

    return status;
}

/*
 * Return the quickfix list title as 'title' in retdict
 */
    static int
qf_getprop_title(qf_info_T *qi, int qf_idx, dict_T *retdict)
{
    char_u	*t;

    t = qi->qf_lists[qf_idx].qf_title;
    if (t == NULL)
	t = (char_u *)"";
    return dict_add_nr_str(retdict, "title", 0L, t);
}

/*
 * Return the quickfix list items/entries as 'items' in retdict
 */
    static int
qf_getprop_items(qf_info_T *qi, int qf_idx, dict_T *retdict)
{
    int		status = OK;
    list_T	*l = list_alloc();
    if (l != NULL)
    {
	(void)get_errorlist(qi, NULL, qf_idx, l);
	dict_add_list(retdict, "items", l);
    }
    else
	status = FAIL;

    return status;
}

/*
 * Return the quickfix list context (if any) as 'context' in retdict.
 */
    static int
qf_getprop_ctx(qf_info_T *qi, int qf_idx, dict_T *retdict)
{
    int		status;
    dictitem_T	*di;

    if (qi->qf_lists[qf_idx].qf_ctx != NULL)
    {
	di = dictitem_alloc((char_u *)"context");
	if (di != NULL)
	{
	    copy_tv(qi->qf_lists[qf_idx].qf_ctx, &di->di_tv);
	    status = dict_add(retdict, di);
	    if (status == FAIL)
		dictitem_free(di);
	}
	else
	    status = FAIL;
    }
    else
	status = dict_add_nr_str(retdict, "context", 0L, (char_u *)"");

    return status;
}

/*
 * Return the quickfix list index as 'idx' in retdict
 */
    static int
qf_getprop_idx(qf_info_T *qi, int qf_idx, dict_T *retdict)
{
    int idx = qi->qf_lists[qf_idx].qf_index;
    if (qi->qf_lists[qf_idx].qf_count == 0)
	/* For empty lists, qf_index is set to 1 */
	idx = 0;
    return dict_add_nr_str(retdict, "idx", idx, NULL);
}

/*
 * Return quickfix/location list details (title) as a
 * dictionary. 'what' contains the details to return. If 'list_idx' is -1,
 * then current list is used. Otherwise the specified list is used.
 */
    int
qf_get_properties(win_T *wp, dict_T *what, dict_T *retdict)
{
    qf_info_T	*qi = &ql_info;
    int		status = OK;
    int		qf_idx;
    dictitem_T	*di;
    int		flags = QF_GETLIST_NONE;

    if ((di = dict_find(what, (char_u *)"lines", -1)) != NULL)
	return qf_get_list_from_lines(what, di, retdict);

    if (wp != NULL)
	qi = GET_LOC_LIST(wp);

    flags = qf_getprop_keys2flags(what);

    if (qi != NULL && qi->qf_listcount != 0)
	qf_idx = qf_getprop_qfidx(qi, what);

    /* List is not present or is empty */
    if (qi == NULL || qi->qf_listcount == 0 || qf_idx == INVALID_QFIDX)
	return qf_getprop_defaults(qi, flags, retdict);

    if (flags & QF_GETLIST_TITLE)
	status = qf_getprop_title(qi, qf_idx, retdict);
    if ((status == OK) && (flags & QF_GETLIST_NR))
	status = dict_add_nr_str(retdict, "nr", qf_idx + 1, NULL);
    if ((status == OK) && (flags & QF_GETLIST_WINID))
	status = dict_add_nr_str(retdict, "winid", qf_winid(qi), NULL);
    if ((status == OK) && (flags & QF_GETLIST_ITEMS))
	status = qf_getprop_items(qi, qf_idx, retdict);
    if ((status == OK) && (flags & QF_GETLIST_CONTEXT))
	status = qf_getprop_ctx(qi, qf_idx, retdict);
    if ((status == OK) && (flags & QF_GETLIST_ID))
	status = dict_add_nr_str(retdict, "id", qi->qf_lists[qf_idx].qf_id,
									 NULL);
    if ((status == OK) && (flags & QF_GETLIST_IDX))
	status = qf_getprop_idx(qi, qf_idx, retdict);
    if ((status == OK) && (flags & QF_GETLIST_SIZE))
	status = dict_add_nr_str(retdict, "size",
					qi->qf_lists[qf_idx].qf_count, NULL);
    if ((status == OK) && (flags & QF_GETLIST_TICK))
	status = dict_add_nr_str(retdict, "changedtick",
				qi->qf_lists[qf_idx].qf_changedtick, NULL);

    return status;
}

/*
 * Add list of entries to quickfix/location list. Each list entry is
 * a dictionary with item information.
 */
    static int
qf_add_entries(
	qf_info_T	*qi,
	int		qf_idx,
	list_T		*list,
	char_u		*title,
	int		action)
{
    listitem_T	*li;
    dict_T	*d;
    char_u	*filename, *module, *pattern, *text, *type;
    int		bufnum;
    long	lnum;
    int		col, nr;
    int		vcol;
    qfline_T	*old_last = NULL;
    int		valid, status;
    int		retval = OK;
    int		did_bufnr_emsg = FALSE;

    if (action == ' ' || qf_idx == qi->qf_listcount)
    {
	/* make place for a new list */
	qf_new_list(qi, title);
	qf_idx = qi->qf_curlist;
    }
    else if (action == 'a' && qi->qf_lists[qf_idx].qf_count > 0)
	/* Adding to existing list, use last entry. */
	old_last = qi->qf_lists[qf_idx].qf_last;
    else if (action == 'r')
    {
	qf_free_items(qi, qf_idx);
	qf_store_title(qi, qf_idx, title);
    }

    for (li = list->lv_first; li != NULL; li = li->li_next)
    {
	if (li->li_tv.v_type != VAR_DICT)
	    continue; /* Skip non-dict items */

	d = li->li_tv.vval.v_dict;
	if (d == NULL)
	    continue;

	filename = get_dict_string(d, (char_u *)"filename", TRUE);
	module = get_dict_string(d, (char_u *)"module", TRUE);
	bufnum = (int)get_dict_number(d, (char_u *)"bufnr");
	lnum = (int)get_dict_number(d, (char_u *)"lnum");
	col = (int)get_dict_number(d, (char_u *)"col");
	vcol = (int)get_dict_number(d, (char_u *)"vcol");
	nr = (int)get_dict_number(d, (char_u *)"nr");
	type = get_dict_string(d, (char_u *)"type", TRUE);
	pattern = get_dict_string(d, (char_u *)"pattern", TRUE);
	text = get_dict_string(d, (char_u *)"text", TRUE);
	if (text == NULL)
	    text = vim_strsave((char_u *)"");

	valid = TRUE;
	if ((filename == NULL && bufnum == 0) || (lnum == 0 && pattern == NULL))
	    valid = FALSE;

	/* Mark entries with non-existing buffer number as not valid. Give the
	 * error message only once. */
	if (bufnum != 0 && (buflist_findnr(bufnum) == NULL))
	{
	    if (!did_bufnr_emsg)
	    {
		did_bufnr_emsg = TRUE;
		EMSGN(_("E92: Buffer %ld not found"), bufnum);
	    }
	    valid = FALSE;
	    bufnum = 0;
	}

	/* If the 'valid' field is present it overrules the detected value. */
	if ((dict_find(d, (char_u *)"valid", -1)) != NULL)
	    valid = (int)get_dict_number(d, (char_u *)"valid");

	status =  qf_add_entry(qi,
			       qf_idx,
			       NULL,	    /* dir */
			       filename,
			       module,
			       bufnum,
			       text,
			       lnum,
			       col,
			       vcol,	    /* vis_col */
			       pattern,	    /* search pattern */
			       nr,
			       type == NULL ? NUL : *type,
			       valid);

	vim_free(filename);
	vim_free(module);
	vim_free(pattern);
	vim_free(text);
	vim_free(type);

	if (status == FAIL)
	{
	    retval = FAIL;
	    break;
	}
    }

    if (qi->qf_lists[qf_idx].qf_index == 0)
	/* no valid entry */
	qi->qf_lists[qf_idx].qf_nonevalid = TRUE;
    else
	qi->qf_lists[qf_idx].qf_nonevalid = FALSE;
    if (action != 'a')
    {
	qi->qf_lists[qf_idx].qf_ptr =
	    qi->qf_lists[qf_idx].qf_start;
	if (qi->qf_lists[qf_idx].qf_count > 0)
	    qi->qf_lists[qf_idx].qf_index = 1;
    }

    /* Don't update the cursor in quickfix window when appending entries */
    qf_update_buffer(qi, old_last);

    return retval;
}

/*
 * Get the quickfix list index from 'nr' or 'id'
 */
    static int
qf_setprop_get_qfidx(
	qf_info_T	*qi,
	dict_T		*what,
	int		action,
	int		*newlist)
{
    dictitem_T	*di;
    int		qf_idx = qi->qf_curlist;    /* default is the current list */

    if ((di = dict_find(what, (char_u *)"nr", -1)) != NULL)
    {
	/* Use the specified quickfix/location list */
	if (di->di_tv.v_type == VAR_NUMBER)
	{
	    /* for zero use the current list */
	    if (di->di_tv.vval.v_number != 0)
		qf_idx = di->di_tv.vval.v_number - 1;

	    if ((action == ' ' || action == 'a') && qf_idx == qi->qf_listcount)
	    {
		/*
		 * When creating a new list, accept qf_idx pointing to the next
		 * non-available list and add the new list at the end of the
		 * stack.
		 */
		*newlist = TRUE;
		qf_idx = qi->qf_listcount > 0 ? qi->qf_listcount - 1 : 0;
	    }
	    else if (qf_idx < 0 || qf_idx >= qi->qf_listcount)
		return INVALID_QFIDX;
	    else if (action != ' ')
		*newlist = FALSE;	/* use the specified list */
	}
	else if (di->di_tv.v_type == VAR_STRING
		&& di->di_tv.vval.v_string != NULL
		&& STRCMP(di->di_tv.vval.v_string, "$") == 0)
	{
	    if (qi->qf_listcount > 0)
		qf_idx = qi->qf_listcount - 1;
	    else if (*newlist)
		qf_idx = 0;
	    else
		return INVALID_QFIDX;
	}
	else
	    return INVALID_QFIDX;
    }

    if (!*newlist && (di = dict_find(what, (char_u *)"id", -1)) != NULL)
    {
	/* Use the quickfix/location list with the specified id */
	if (di->di_tv.v_type != VAR_NUMBER)
	    return INVALID_QFIDX;

	return qf_id2nr(qi, di->di_tv.vval.v_number);
    }

    return qf_idx;
}

/*
 * Set the quickfix list title.
 */
    static int
qf_setprop_title(qf_info_T *qi, int qf_idx, dict_T *what, dictitem_T *di)
{
    if (di->di_tv.v_type != VAR_STRING)
	return FAIL;

    vim_free(qi->qf_lists[qf_idx].qf_title);
    qi->qf_lists[qf_idx].qf_title =
	get_dict_string(what, (char_u *)"title", TRUE);
    if (qf_idx == qi->qf_curlist)
	qf_update_win_titlevar(qi);

    return OK;
}

/*
 * Set quickfix list items/entries.
 */
    static int
qf_setprop_items(qf_info_T *qi, int qf_idx, dictitem_T *di, int action)
{
    int		retval = FAIL;
    char_u	*title_save;

    if (di->di_tv.v_type != VAR_LIST)
	return FAIL;

    title_save = vim_strsave(qi->qf_lists[qf_idx].qf_title);
    retval = qf_add_entries(qi, qf_idx, di->di_tv.vval.v_list,
	    title_save, action == ' ' ? 'a' : action);
    if (action == 'r')
    {
	/*
	 * When replacing the quickfix list entries using
	 * qf_add_entries(), the title is set with a ':' prefix.
	 * Restore the title with the saved title.
	 */
	vim_free(qi->qf_lists[qf_idx].qf_title);
	qi->qf_lists[qf_idx].qf_title = vim_strsave(title_save);
    }
    vim_free(title_save);

    return retval;
}

/*
 * Set quickfix list items/entries from a list of lines.
 */
    static int
qf_setprop_items_from_lines(
	qf_info_T	*qi,
	int		qf_idx,
	dict_T		*what,
	dictitem_T	*di,
	int		action)
{
    char_u	*errorformat = p_efm;
    dictitem_T	*efm_di;
    int		retval = FAIL;

    /* Use the user supplied errorformat settings (if present) */
    if ((efm_di = dict_find(what, (char_u *)"efm", -1)) != NULL)
    {
	if (efm_di->di_tv.v_type != VAR_STRING ||
		efm_di->di_tv.vval.v_string == NULL)
	    return FAIL;
	errorformat = efm_di->di_tv.vval.v_string;
    }

    /* Only a List value is supported */
    if (di->di_tv.v_type != VAR_LIST || di->di_tv.vval.v_list == NULL)
	return FAIL;

    if (action == 'r')
	qf_free_items(qi, qf_idx);
    if (qf_init_ext(qi, qf_idx, NULL, NULL, &di->di_tv, errorformat,
		FALSE, (linenr_T)0, (linenr_T)0, NULL, NULL) > 0)
	retval = OK;

    return retval;
}

/*
 * Set quickfix list context.
 */
    static int
qf_setprop_context(qf_info_T *qi, int qf_idx, dictitem_T *di)
{
    typval_T	*ctx;

    free_tv(qi->qf_lists[qf_idx].qf_ctx);
    ctx =  alloc_tv();
    if (ctx != NULL)
	copy_tv(&di->di_tv, ctx);
    qi->qf_lists[qf_idx].qf_ctx = ctx;

    return OK;
}

/*
 * Set quickfix/location list properties (title, items, context).
 * Also used to add items from parsing a list of lines.
 * Used by the setqflist() and setloclist() Vim script functions.
 */
    static int
qf_set_properties(qf_info_T *qi, dict_T *what, int action, char_u *title)
{
    dictitem_T	*di;
    int		retval = FAIL;
    int		qf_idx;
    int		newlist = FALSE;

    if (action == ' ' || qi->qf_curlist == qi->qf_listcount)
	newlist = TRUE;

    qf_idx = qf_setprop_get_qfidx(qi, what, action, &newlist);
    if (qf_idx == INVALID_QFIDX)	/* List not found */
	return FAIL;

    if (newlist)
    {
	qi->qf_curlist = qf_idx;
	qf_new_list(qi, title);
	qf_idx = qi->qf_curlist;
    }

    if ((di = dict_find(what, (char_u *)"title", -1)) != NULL)
	retval = qf_setprop_title(qi, qf_idx, what, di);
    if ((di = dict_find(what, (char_u *)"items", -1)) != NULL)
	retval = qf_setprop_items(qi, qf_idx, di, action);
    if ((di = dict_find(what, (char_u *)"lines", -1)) != NULL)
	retval = qf_setprop_items_from_lines(qi, qf_idx, what, di, action);
    if ((di = dict_find(what, (char_u *)"context", -1)) != NULL)
	retval = qf_setprop_context(qi, qf_idx, di);

    if (retval == OK)
	qf_list_changed(qi, qf_idx);

    return retval;
}

/*
 * Find the non-location list window with the specified location list.
 */
    static win_T *
find_win_with_ll(qf_info_T *qi)
{
    win_T	*wp = NULL;

    FOR_ALL_WINDOWS(wp)
	if ((wp->w_llist == qi) && !bt_quickfix(wp->w_buffer))
	    return wp;

    return NULL;
}

/*
 * Free the entire quickfix/location list stack.
 * If the quickfix/location list window is open, then clear it.
 */
    static void
qf_free_stack(win_T *wp, qf_info_T *qi)
{
    win_T	*qfwin = qf_find_win(qi);
    win_T	*llwin = NULL;
    win_T	*orig_wp = wp;

    if (qfwin != NULL)
    {
	/* If the quickfix/location list window is open, then clear it */
	if (qi->qf_curlist < qi->qf_listcount)
	    qf_free(qi, qi->qf_curlist);
	qf_update_buffer(qi, NULL);
    }

    if (wp != NULL && IS_LL_WINDOW(wp))
    {
	/* If in the location list window, then use the non-location list
	 * window with this location list (if present)
	 */
	llwin = find_win_with_ll(qi);
	if (llwin != NULL)
	    wp = llwin;
    }

    qf_free_all(wp);
    if (wp == NULL)
    {
	/* quickfix list */
	qi->qf_curlist = 0;
	qi->qf_listcount = 0;
    }
    else if (IS_LL_WINDOW(orig_wp))
    {
	/* If the location list window is open, then create a new empty
	 * location list */
	qf_info_T *new_ll = ll_new_list();

	/* first free the list reference in the location list window */
	ll_free_all(&orig_wp->w_llist_ref);

	orig_wp->w_llist_ref = new_ll;
	if (llwin != NULL)
	{
	    llwin->w_llist = new_ll;
	    new_ll->qf_refcount++;
	}
    }
}

/*
 * Populate the quickfix list with the items supplied in the list
 * of dictionaries. "title" will be copied to w:quickfix_title.
 * "action" is 'a' for add, 'r' for replace.  Otherwise create a new list.
 */
    int
set_errorlist(
	win_T	*wp,
	list_T	*list,
	int	action,
	char_u	*title,
	dict_T	*what)
{
    qf_info_T	*qi = &ql_info;
    int		retval = OK;

    if (wp != NULL)
    {
	qi = ll_get_or_alloc_list(wp);
	if (qi == NULL)
	    return FAIL;
    }

    if (action == 'f')
    {
	/* Free the entire quickfix or location list stack */
	qf_free_stack(wp, qi);
    }
    else if (what != NULL)
	retval = qf_set_properties(qi, what, action, title);
    else
    {
	retval = qf_add_entries(qi, qi->qf_curlist, list, title, action);
	if (retval == OK)
	    qf_list_changed(qi, qi->qf_curlist);
    }

    return retval;
}

/*
 * Mark the context as in use for all the lists in a quickfix stack.
 */
    static int
mark_quickfix_ctx(qf_info_T *qi, int copyID)
{
    int		i;
    int		abort = FALSE;
    typval_T	*ctx;

    for (i = 0; i < LISTCOUNT && !abort; ++i)
    {
	ctx = qi->qf_lists[i].qf_ctx;
	if (ctx != NULL && ctx->v_type != VAR_NUMBER
		&& ctx->v_type != VAR_STRING && ctx->v_type != VAR_FLOAT)
	    abort = set_ref_in_item(ctx, copyID, NULL, NULL);
    }

    return abort;
}

/*
 * Mark the context of the quickfix list and the location lists (if present) as
 * "in use". So that garbage collection doesn't free the context.
 */
    int
set_ref_in_quickfix(int copyID)
{
    int		abort = FALSE;
    tabpage_T	*tp;
    win_T	*win;

    abort = mark_quickfix_ctx(&ql_info, copyID);
    if (abort)
	return abort;

    FOR_ALL_TAB_WINDOWS(tp, win)
    {
	if (win->w_llist != NULL)
	{
	    abort = mark_quickfix_ctx(win->w_llist, copyID);
	    if (abort)
		return abort;
	}
	if (IS_LL_WINDOW(win) && (win->w_llist_ref->qf_refcount == 1))
	{
	    /* In a location list window and none of the other windows is
	     * referring to this location list. Mark the location list
	     * context as still in use.
	     */
	    abort = mark_quickfix_ctx(win->w_llist_ref, copyID);
	    if (abort)
		return abort;
	}
    }

    return abort;
}
#endif

/*
 * ":[range]cbuffer [bufnr]" command.
 * ":[range]caddbuffer [bufnr]" command.
 * ":[range]cgetbuffer [bufnr]" command.
 * ":[range]lbuffer [bufnr]" command.
 * ":[range]laddbuffer [bufnr]" command.
 * ":[range]lgetbuffer [bufnr]" command.
 */
    void
ex_cbuffer(exarg_T *eap)
{
    buf_T	*buf = NULL;
    qf_info_T	*qi = &ql_info;
    char_u	*au_name = NULL;
    int		res;

    switch (eap->cmdidx)
    {
	case CMD_cbuffer:	au_name = (char_u *)"cbuffer"; break;
	case CMD_cgetbuffer:	au_name = (char_u *)"cgetbuffer"; break;
	case CMD_caddbuffer:	au_name = (char_u *)"caddbuffer"; break;
	case CMD_lbuffer:	au_name = (char_u *)"lbuffer"; break;
	case CMD_lgetbuffer:	au_name = (char_u *)"lgetbuffer"; break;
	case CMD_laddbuffer:	au_name = (char_u *)"laddbuffer"; break;
	default: break;
    }
    if (au_name != NULL && apply_autocmds(EVENT_QUICKFIXCMDPRE, au_name,
					       curbuf->b_fname, TRUE, curbuf))
    {
#ifdef FEAT_EVAL
	if (aborting())
	    return;
#endif
    }

    /* Must come after autocommands. */
    if (eap->cmdidx == CMD_lbuffer
	    || eap->cmdidx == CMD_lgetbuffer
	    || eap->cmdidx == CMD_laddbuffer)
    {
	qi = ll_get_or_alloc_list(curwin);
	if (qi == NULL)
	    return;
    }

    if (*eap->arg == NUL)
	buf = curbuf;
    else if (*skipwhite(skipdigits(eap->arg)) == NUL)
	buf = buflist_findnr(atoi((char *)eap->arg));
    if (buf == NULL)
	EMSG(_(e_invarg));
    else if (buf->b_ml.ml_mfp == NULL)
	EMSG(_("E681: Buffer is not loaded"));
    else
    {
	if (eap->addr_count == 0)
	{
	    eap->line1 = 1;
	    eap->line2 = buf->b_ml.ml_line_count;
	}
	if (eap->line1 < 1 || eap->line1 > buf->b_ml.ml_line_count
		|| eap->line2 < 1 || eap->line2 > buf->b_ml.ml_line_count)
	    EMSG(_(e_invrange));
	else
	{
	    char_u *qf_title = qf_cmdtitle(*eap->cmdlinep);

	    if (buf->b_sfname)
	    {
		vim_snprintf((char *)IObuff, IOSIZE, "%s (%s)",
				     (char *)qf_title, (char *)buf->b_sfname);
		qf_title = IObuff;
	    }

	    res = qf_init_ext(qi, qi->qf_curlist, NULL, buf, NULL, p_efm,
			    (eap->cmdidx != CMD_caddbuffer
			     && eap->cmdidx != CMD_laddbuffer),
						   eap->line1, eap->line2,
						   qf_title, NULL);
	    if (res >= 0)
		qf_list_changed(qi, qi->qf_curlist);
	    if (au_name != NULL)
		apply_autocmds(EVENT_QUICKFIXCMDPOST, au_name,
						curbuf->b_fname, TRUE, curbuf);
	    if (res > 0 && (eap->cmdidx == CMD_cbuffer ||
						eap->cmdidx == CMD_lbuffer))
		qf_jump(qi, 0, 0, eap->forceit);  /* display first error */
	}
    }
}

#if defined(FEAT_EVAL) || defined(PROTO)
/*
 * ":cexpr {expr}", ":cgetexpr {expr}", ":caddexpr {expr}" command.
 * ":lexpr {expr}", ":lgetexpr {expr}", ":laddexpr {expr}" command.
 */
    void
ex_cexpr(exarg_T *eap)
{
    typval_T	*tv;
    qf_info_T	*qi = &ql_info;
    char_u	*au_name = NULL;
    int		res;

    switch (eap->cmdidx)
    {
	case CMD_cexpr:	    au_name = (char_u *)"cexpr"; break;
	case CMD_cgetexpr:  au_name = (char_u *)"cgetexpr"; break;
	case CMD_caddexpr:  au_name = (char_u *)"caddexpr"; break;
	case CMD_lexpr:	    au_name = (char_u *)"lexpr"; break;
	case CMD_lgetexpr:  au_name = (char_u *)"lgetexpr"; break;
	case CMD_laddexpr:  au_name = (char_u *)"laddexpr"; break;
	default: break;
    }
    if (au_name != NULL && apply_autocmds(EVENT_QUICKFIXCMDPRE, au_name,
					       curbuf->b_fname, TRUE, curbuf))
    {
#ifdef FEAT_EVAL
	if (aborting())
	    return;
#endif
    }

    if (eap->cmdidx == CMD_lexpr
	    || eap->cmdidx == CMD_lgetexpr
	    || eap->cmdidx == CMD_laddexpr)
    {
	qi = ll_get_or_alloc_list(curwin);
	if (qi == NULL)
	    return;
    }

    /* Evaluate the expression.  When the result is a string or a list we can
     * use it to fill the errorlist. */
    tv = eval_expr(eap->arg, NULL);
    if (tv != NULL)
    {
	if ((tv->v_type == VAR_STRING && tv->vval.v_string != NULL)
		|| (tv->v_type == VAR_LIST && tv->vval.v_list != NULL))
	{
	    res = qf_init_ext(qi, qi->qf_curlist, NULL, NULL, tv, p_efm,
			    (eap->cmdidx != CMD_caddexpr
			     && eap->cmdidx != CMD_laddexpr),
				 (linenr_T)0, (linenr_T)0,
				 qf_cmdtitle(*eap->cmdlinep), NULL);
	    if (res >= 0)
		qf_list_changed(qi, qi->qf_curlist);
	    if (au_name != NULL)
		apply_autocmds(EVENT_QUICKFIXCMDPOST, au_name,
						curbuf->b_fname, TRUE, curbuf);
	    if (res > 0 && (eap->cmdidx == CMD_cexpr ||
						eap->cmdidx == CMD_lexpr))
		qf_jump(qi, 0, 0, eap->forceit);  /* display first error */
	}
	else
	    EMSG(_("E777: String or List expected"));
	free_tv(tv);
    }
}
#endif

/*
 * Get the location list for ":lhelpgrep"
 */
    static qf_info_T *
hgr_get_ll(int *new_ll)
{
    win_T	*wp;
    qf_info_T	*qi;

    /* If the current window is a help window, then use it */
    if (bt_help(curwin->w_buffer))
	wp = curwin;
    else
	/* Find an existing help window */
	wp = qf_find_help_win();

    if (wp == NULL)	    /* Help window not found */
	qi = NULL;
    else
	qi = wp->w_llist;

    if (qi == NULL)
    {
	/* Allocate a new location list for help text matches */
	if ((qi = ll_new_list()) == NULL)
	    return NULL;
	*new_ll = TRUE;
    }

    return qi;
}

/*
 * Search for a pattern in a help file.
 */
    static void
hgr_search_file(
	qf_info_T *qi,
	char_u *fname,
#ifdef FEAT_MBYTE
	vimconv_T *p_vc,
#endif
	regmatch_T *p_regmatch)
{
    FILE	*fd;
    long	lnum;

    fd = mch_fopen((char *)fname, "r");
    if (fd == NULL)
	return;

    lnum = 1;
    while (!vim_fgets(IObuff, IOSIZE, fd) && !got_int)
    {
	char_u    *line = IObuff;
#ifdef FEAT_MBYTE
	/* Convert a line if 'encoding' is not utf-8 and
	 * the line contains a non-ASCII character. */
	if (p_vc->vc_type != CONV_NONE
		&& has_non_ascii(IObuff))
	{
	    line = string_convert(p_vc, IObuff, NULL);
	    if (line == NULL)
		line = IObuff;
	}
#endif

	if (vim_regexec(p_regmatch, line, (colnr_T)0))
	{
	    int	l = (int)STRLEN(line);

	    /* remove trailing CR, LF, spaces, etc. */
	    while (l > 0 && line[l - 1] <= ' ')
		line[--l] = NUL;

	    if (qf_add_entry(qi,
			qi->qf_curlist,
			NULL,	/* dir */
			fname,
			NULL,
			0,
			line,
			lnum,
			(int)(p_regmatch->startp[0] - line)
			+ 1, /* col */
			FALSE,	/* vis_col */
			NULL,	/* search pattern */
			0,		/* nr */
			1,		/* type */
			TRUE	/* valid */
			) == FAIL)
	    {
		got_int = TRUE;
#ifdef FEAT_MBYTE
		if (line != IObuff)
		    vim_free(line);
#endif
		break;
	    }
	}
#ifdef FEAT_MBYTE
	if (line != IObuff)
	    vim_free(line);
#endif
	++lnum;
	line_breakcheck();
    }
    fclose(fd);
}

/*
 * Search for a pattern in all the help files in the doc directory under
 * the given directory.
 */
    static void
hgr_search_files_in_dir(
	qf_info_T *qi,
	char_u *dirname,
	regmatch_T *p_regmatch
#ifdef FEAT_MBYTE
	, vimconv_T *p_vc
#endif
#ifdef FEAT_MULTI_LANG
	, char_u *lang
#endif
	)
{
    int		fcount;
    char_u	**fnames;
    int		fi;

    /* Find all "*.txt" and "*.??x" files in the "doc" directory. */
    add_pathsep(dirname);
    STRCAT(dirname, "doc/*.\\(txt\\|??x\\)");
    if (gen_expand_wildcards(1, &dirname, &fcount,
		&fnames, EW_FILE|EW_SILENT) == OK
	    && fcount > 0)
    {
	for (fi = 0; fi < fcount && !got_int; ++fi)
	{
#ifdef FEAT_MULTI_LANG
	    /* Skip files for a different language. */
	    if (lang != NULL
		    && STRNICMP(lang, fnames[fi]
				    + STRLEN(fnames[fi]) - 3, 2) != 0
		    && !(STRNICMP(lang, "en", 2) == 0
			&& STRNICMP("txt", fnames[fi]
			    + STRLEN(fnames[fi]) - 3, 3) == 0))
		continue;
#endif

	    hgr_search_file(qi, fnames[fi],
#ifdef FEAT_MBYTE
		    p_vc,
#endif
		    p_regmatch);
	}
	FreeWild(fcount, fnames);
    }
}

/*
 * Search for a pattern in all the help files in the 'runtimepath'
 * and add the matches to a quickfix list.
 * 'arg' is the language specifier.  If supplied, then only matches in the
 * specified language are found.
 */
    static void
hgr_search_in_rtp(qf_info_T *qi, regmatch_T *p_regmatch, char_u *arg)
{
    char_u	*p;
#ifdef FEAT_MULTI_LANG
    char_u	*lang;
#endif

#ifdef FEAT_MBYTE
    vimconv_T	vc;

    /* Help files are in utf-8 or latin1, convert lines when 'encoding'
     * differs. */
    vc.vc_type = CONV_NONE;
    if (!enc_utf8)
	convert_setup(&vc, (char_u *)"utf-8", p_enc);
#endif

#ifdef FEAT_MULTI_LANG
    /* Check for a specified language */
    lang = check_help_lang(arg);
#endif

    /* Go through all the directories in 'runtimepath' */
    p = p_rtp;
    while (*p != NUL && !got_int)
    {
	copy_option_part(&p, NameBuff, MAXPATHL, ",");

	hgr_search_files_in_dir(qi, NameBuff, p_regmatch
#ifdef FEAT_MBYTE
		, &vc
#endif
#ifdef FEAT_MULTI_LANG
		, lang
#endif
		);
    }

#ifdef FEAT_MBYTE
    if (vc.vc_type != CONV_NONE)
	convert_setup(&vc, NULL, NULL);
#endif
}

/*
 * ":helpgrep {pattern}"
 */
    void
ex_helpgrep(exarg_T *eap)
{
    regmatch_T	regmatch;
    char_u	*save_cpo;
    qf_info_T	*qi = &ql_info;
    int		new_qi = FALSE;
    char_u	*au_name =  NULL;

    switch (eap->cmdidx)
    {
	case CMD_helpgrep:  au_name = (char_u *)"helpgrep"; break;
	case CMD_lhelpgrep: au_name = (char_u *)"lhelpgrep"; break;
	default: break;
    }
    if (au_name != NULL && apply_autocmds(EVENT_QUICKFIXCMDPRE, au_name,
					       curbuf->b_fname, TRUE, curbuf))
    {
#ifdef FEAT_EVAL
	if (aborting())
	    return;
#endif
    }

    /* Make 'cpoptions' empty, the 'l' flag should not be used here. */
    save_cpo = p_cpo;
    p_cpo = empty_option;

    if (eap->cmdidx == CMD_lhelpgrep)
    {
	qi = hgr_get_ll(&new_qi);
	if (qi == NULL)
	    return;
    }

    regmatch.regprog = vim_regcomp(eap->arg, RE_MAGIC + RE_STRING);
    regmatch.rm_ic = FALSE;
    if (regmatch.regprog != NULL)
    {
	/* create a new quickfix list */
	qf_new_list(qi, qf_cmdtitle(*eap->cmdlinep));

	hgr_search_in_rtp(qi, &regmatch, eap->arg);

	vim_regfree(regmatch.regprog);

	qi->qf_lists[qi->qf_curlist].qf_nonevalid = FALSE;
	qi->qf_lists[qi->qf_curlist].qf_ptr =
	    qi->qf_lists[qi->qf_curlist].qf_start;
	qi->qf_lists[qi->qf_curlist].qf_index = 1;
    }

    if (p_cpo == empty_option)
	p_cpo = save_cpo;
    else
	/* Darn, some plugin changed the value. */
	free_string_option(save_cpo);

    qf_list_changed(qi, qi->qf_curlist);
    qf_update_buffer(qi, NULL);

    if (au_name != NULL)
    {
	apply_autocmds(EVENT_QUICKFIXCMDPOST, au_name,
					       curbuf->b_fname, TRUE, curbuf);
	if (!new_qi && qi != &ql_info && qf_find_buf(qi) == NULL)
	    /* autocommands made "qi" invalid */
	    return;
    }

    /* Jump to first match. */
    if (qi->qf_lists[qi->qf_curlist].qf_count > 0)
	qf_jump(qi, 0, 0, FALSE);
    else
	EMSG2(_(e_nomatch2), eap->arg);

    if (eap->cmdidx == CMD_lhelpgrep)
    {
	/* If the help window is not opened or if it already points to the
	 * correct location list, then free the new location list. */
	if (!bt_help(curwin->w_buffer) || curwin->w_llist == qi)
	{
	    if (new_qi)
		ll_free_all(&qi);
	}
	else if (curwin->w_llist == NULL)
	    curwin->w_llist = qi;
    }
}

#endif /* FEAT_QUICKFIX */
