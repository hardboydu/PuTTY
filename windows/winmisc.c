/*
 * winmisc.c: miscellaneous Windows-specific things
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "putty.h"
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>
#include <ctype.h>

DWORD osMajorVersion, osMinorVersion, osPlatformId;

char *platform_get_x_display(void) {
    /* We may as well check for DISPLAY in case it's useful. */
    return dupstr(getenv("DISPLAY"));
}

Filename *filename_from_str(const char *str)
{
    Filename *ret = snew(Filename);
    ret->path = dupstr(str);
    return ret;
}

Filename *filename_copy(const Filename *fn)
{
    return filename_from_str(fn->path);
}

const char *filename_to_str(const Filename *fn)
{
    return fn->path;
}

int filename_equal(const Filename *f1, const Filename *f2)
{
    return !strcmp(f1->path, f2->path);
}

int filename_is_null(const Filename *fn)
{
    return !*fn->path;
}

void filename_free(Filename *fn)
{
    sfree(fn->path);
    sfree(fn);
}

void filename_serialise(BinarySink *bs, const Filename *f)
{
    put_asciz(bs, f->path);
}
Filename *filename_deserialise(BinarySource *src)
{
    return filename_from_str(get_asciz(src));
}

char filename_char_sanitise(char c)
{
    if (strchr("<>:\"/\\|?*", c))
        return '.';
    return c;
}

#ifndef NO_SECUREZEROMEMORY
/*
 * Windows implementation of smemclr (see misc.c) using SecureZeroMemory.
 */
void smemclr(void *b, size_t n) {
    if (b && n > 0)
        SecureZeroMemory(b, n);
}
#endif

char *get_username(void)
{
    DWORD namelen;
    char *user;
    int got_username = FALSE;
    DECL_WINDOWS_FUNCTION(static, BOOLEAN, GetUserNameExA,
			  (EXTENDED_NAME_FORMAT, LPSTR, PULONG));

    {
	static int tried_usernameex = FALSE;
	if (!tried_usernameex) {
	    /* Not available on Win9x, so load dynamically */
	    HMODULE secur32 = load_system32_dll("secur32.dll");
	    /* If MIT Kerberos is installed, the following call to
	       GET_WINDOWS_FUNCTION makes Windows implicitly load
	       sspicli.dll WITHOUT proper path sanitizing, so better
	       load it properly before */
	    HMODULE sspicli = load_system32_dll("sspicli.dll");
            (void)sspicli; /* squash compiler warning about unused variable */
	    GET_WINDOWS_FUNCTION(secur32, GetUserNameExA);
	    tried_usernameex = TRUE;
	}
    }

    if (p_GetUserNameExA) {
	/*
	 * If available, use the principal -- this avoids the problem
	 * that the local username is case-insensitive but Kerberos
	 * usernames are case-sensitive.
	 */

	/* Get the length */
	namelen = 0;
	(void) p_GetUserNameExA(NameUserPrincipal, NULL, &namelen);

	user = snewn(namelen, char);
	got_username = p_GetUserNameExA(NameUserPrincipal, user, &namelen);
	if (got_username) {
	    char *p = strchr(user, '@');
	    if (p) *p = 0;
	} else {
	    sfree(user);
	}
    }

    if (!got_username) {
	/* Fall back to local user name */
	namelen = 0;
	if (GetUserName(NULL, &namelen) == FALSE) {
	    /*
	     * Apparently this doesn't work at least on Windows XP SP2.
	     * Thus assume a maximum of 256. It will fail again if it
	     * doesn't fit.
	     */
	    namelen = 256;
	}

	user = snewn(namelen, char);
	got_username = GetUserName(user, &namelen);
	if (!got_username) {
	    sfree(user);
	}
    }

    return got_username ? user : NULL;
}

void dll_hijacking_protection(void)
{
    /*
     * If the OS provides it, call SetDefaultDllDirectories() to
     * prevent DLLs from being loaded from the directory containing
     * our own binary, and instead only load from system32.
     *
     * This is a protection against hijacking attacks, if someone runs
     * PuTTY directly from their web browser's download directory
     * having previously been enticed into clicking on an unwise link
     * that downloaded a malicious DLL to the same directory under one
     * of various magic names that seem to be things that standard
     * Windows DLLs delegate to.
     *
     * It shouldn't break deliberate loading of user-provided DLLs
     * such as GSSAPI providers, because those are specified by their
     * full pathname by the user-provided configuration.
     */
    static HMODULE kernel32_module;
    DECL_WINDOWS_FUNCTION(static, BOOL, SetDefaultDllDirectories, (DWORD));

    if (!kernel32_module) {
        kernel32_module = load_system32_dll("kernel32.dll");
#if (defined _MSC_VER && _MSC_VER < 1900) || defined COVERITY
        /* For older Visual Studio, and also for the system I
         * currently use for Coveritying the Windows code, this
         * function isn't available in the header files to
         * type-check */
        GET_WINDOWS_FUNCTION_NO_TYPECHECK(
            kernel32_module, SetDefaultDllDirectories);
#else
        GET_WINDOWS_FUNCTION(kernel32_module, SetDefaultDllDirectories);
#endif
    }

    if (p_SetDefaultDllDirectories) {
        /* LOAD_LIBRARY_SEARCH_SYSTEM32 and explicitly specified
         * directories only */
        p_SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                   LOAD_LIBRARY_SEARCH_USER_DIRS);
    }
}

void init_winver(void)
{
    OSVERSIONINFO osVersion;
    static HMODULE kernel32_module;
    DECL_WINDOWS_FUNCTION(static, BOOL, GetVersionExA, (LPOSVERSIONINFO));

    if (!kernel32_module) {
        kernel32_module = load_system32_dll("kernel32.dll");
        /* Deliberately don't type-check this function, because that
         * would involve using its declaration in a header file which
         * triggers a deprecation warning. I know it's deprecated (see
         * below) and don't need telling. */
        GET_WINDOWS_FUNCTION_NO_TYPECHECK(kernel32_module, GetVersionExA);
    }

    ZeroMemory(&osVersion, sizeof(osVersion));
    osVersion.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
    if (p_GetVersionExA && p_GetVersionExA(&osVersion)) {
        osMajorVersion = osVersion.dwMajorVersion;
        osMinorVersion = osVersion.dwMinorVersion;
        osPlatformId = osVersion.dwPlatformId;
    } else {
        /*
         * GetVersionEx is deprecated, so allow for it perhaps going
         * away in future API versions. If it's not there, simply
         * assume that's because Windows is too _new_, so fill in the
         * variables we care about to a value that will always compare
         * higher than any given test threshold.
         *
         * Normally we should be checking against the presence of a
         * specific function if possible in any case.
         */
        osMajorVersion = osMinorVersion = UINT_MAX; /* a very high number */
        osPlatformId = VER_PLATFORM_WIN32_NT; /* not Win32s or Win95-like */
    }
}

HMODULE load_system32_dll(const char *libname)
{
    /*
     * Wrapper function to load a DLL out of c:\windows\system32
     * without going through the full DLL search path. (Hence no
     * attack is possible by placing a substitute DLL earlier on that
     * path.)
     */
    static char *sysdir = NULL;
    char *fullpath;
    HMODULE ret;

    if (!sysdir) {
	int size = 0, len;
	do {
	    size = 3*size/2 + 512;
	    sysdir = sresize(sysdir, size, char);
	    len = GetSystemDirectory(sysdir, size);
	} while (len >= size);
    }

    fullpath = dupcat(sysdir, "\\", libname, NULL);
    ret = LoadLibrary(fullpath);
    sfree(fullpath);
    return ret;
}

/*
 * A tree234 containing mappings from system error codes to strings.
 */

struct errstring {
    int error;
    char *text;
};

static int errstring_find(void *av, void *bv)
{
    int *a = (int *)av;
    struct errstring *b = (struct errstring *)bv;
    if (*a < b->error)
        return -1;
    if (*a > b->error)
        return +1;
    return 0;
}
static int errstring_compare(void *av, void *bv)
{
    struct errstring *a = (struct errstring *)av;
    return errstring_find(&a->error, bv);
}

static tree234 *errstrings = NULL;

const char *win_strerror(int error)
{
    struct errstring *es;

    if (!errstrings)
        errstrings = newtree234(errstring_compare);

    es = find234(errstrings, &error, errstring_find);

    if (!es) {
        char msgtext[65536]; /* maximum size for FormatMessage is 64K */

        es = snew(struct errstring);
        es->error = error;
        if (!FormatMessage((FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS), NULL, error,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           msgtext, lenof(msgtext)-1, NULL)) {
            sprintf(msgtext,
                    "(unable to format: FormatMessage returned %u)",
                    (unsigned int)GetLastError());
        } else {
            int len = strlen(msgtext);
            if (len > 0 && msgtext[len-1] == '\n')
                msgtext[len-1] = '\0';
        }
        es->text = dupprintf("Error %d: %s", error, msgtext);
        add234(errstrings, es);
    }

    return es->text;
}

#ifndef __MINGW32__

/* This function is in the public domain. */
int strcasecmp( char const* s1, char const* s2)
{
    for (;;) {
        int c1 = tolower( *((unsigned char*) s1++));
        int c2 = tolower( *((unsigned char*) s2++));

        if ((c1 != c2) || (c1 == '\0')) {
            return( c1 - c2);
        }
    }
}

/* This function is in the public domain. */
int strncasecmp( char const* s1, char const* s2, size_t n)
{
    for (; n != 0; --n) {
        int c1 = tolower( *((unsigned char*) s1++));
        int c2 = tolower( *((unsigned char*) s2++));

        if ((c1 != c2) || (c1 == '\0')) {
            return( c1 - c2);
        }
    }

    return( 0);
}
#endif

#ifdef DEBUG
static FILE *debug_fp = NULL;
static HANDLE debug_hdl = INVALID_HANDLE_VALUE;
static int debug_got_console = 0;

void dputs(const char *buf)
{
    DWORD dw;

    if (!debug_got_console) {
	if (AllocConsole()) {
	    debug_got_console = 1;
	    debug_hdl = GetStdHandle(STD_OUTPUT_HANDLE);
	}
    }
    if (!debug_fp) {
	debug_fp = fopen("debug.log", "w");
    }

    if (debug_hdl != INVALID_HANDLE_VALUE) {
	WriteFile(debug_hdl, buf, strlen(buf), &dw, NULL);
    }
    fputs(buf, debug_fp);
    fflush(debug_fp);
}
#endif

#ifdef MINEFIELD
/*
 * Minefield - a Windows equivalent for Electric Fence
 */

#define PAGESIZE 4096

/*
 * Design:
 * 
 * We start by reserving as much virtual address space as Windows
 * will sensibly (or not sensibly) let us have. We flag it all as
 * invalid memory.
 * 
 * Any allocation attempt is satisfied by committing one or more
 * pages, with an uncommitted page on either side. The returned
 * memory region is jammed up against the _end_ of the pages.
 * 
 * Freeing anything causes instantaneous decommitment of the pages
 * involved, so stale pointers are caught as soon as possible.
 */

static int minefield_initialised = 0;
static void *minefield_region = NULL;
static long minefield_size = 0;
static long minefield_npages = 0;
static long minefield_curpos = 0;
static unsigned short *minefield_admin = NULL;
static void *minefield_pages = NULL;

static void minefield_admin_hide(int hide)
{
    int access = hide ? PAGE_NOACCESS : PAGE_READWRITE;
    VirtualProtect(minefield_admin, minefield_npages * 2, access, NULL);
}

static void minefield_init(void)
{
    int size;
    int admin_size;
    int i;

    for (size = 0x40000000; size > 0; size = ((size >> 3) * 7) & ~0xFFF) {
	minefield_region = VirtualAlloc(NULL, size,
					MEM_RESERVE, PAGE_NOACCESS);
	if (minefield_region)
	    break;
    }
    minefield_size = size;

    /*
     * Firstly, allocate a section of that to be the admin block.
     * We'll need a two-byte field for each page.
     */
    minefield_admin = minefield_region;
    minefield_npages = minefield_size / PAGESIZE;
    admin_size = (minefield_npages * 2 + PAGESIZE - 1) & ~(PAGESIZE - 1);
    minefield_npages = (minefield_size - admin_size) / PAGESIZE;
    minefield_pages = (char *) minefield_region + admin_size;

    /*
     * Commit the admin region.
     */
    VirtualAlloc(minefield_admin, minefield_npages * 2,
		 MEM_COMMIT, PAGE_READWRITE);

    /*
     * Mark all pages as unused (0xFFFF).
     */
    for (i = 0; i < minefield_npages; i++)
	minefield_admin[i] = 0xFFFF;

    /*
     * Hide the admin region.
     */
    minefield_admin_hide(1);

    minefield_initialised = 1;
}

static void minefield_bomb(void)
{
    div(1, *(int *) minefield_pages);
}

static void *minefield_alloc(int size)
{
    int npages;
    int pos, lim, region_end, region_start;
    int start;
    int i;

    npages = (size + PAGESIZE - 1) / PAGESIZE;

    minefield_admin_hide(0);

    /*
     * Search from current position until we find a contiguous
     * bunch of npages+2 unused pages.
     */
    pos = minefield_curpos;
    lim = minefield_npages;
    while (1) {
	/* Skip over used pages. */
	while (pos < lim && minefield_admin[pos] != 0xFFFF)
	    pos++;
	/* Count unused pages. */
	start = pos;
	while (pos < lim && pos - start < npages + 2 &&
	       minefield_admin[pos] == 0xFFFF)
	    pos++;
	if (pos - start == npages + 2)
	    break;
	/* If we've reached the limit, reset the limit or stop. */
	if (pos >= lim) {
	    if (lim == minefield_npages) {
		/* go round and start again at zero */
		lim = minefield_curpos;
		pos = 0;
	    } else {
		minefield_admin_hide(1);
		return NULL;
	    }
	}
    }

    minefield_curpos = pos - 1;

    /*
     * We have npages+2 unused pages starting at start. We leave
     * the first and last of these alone and use the rest.
     */
    region_end = (start + npages + 1) * PAGESIZE;
    region_start = region_end - size;
    /* FIXME: could align here if we wanted */

    /*
     * Update the admin region.
     */
    for (i = start + 2; i < start + npages + 1; i++)
	minefield_admin[i] = 0xFFFE;   /* used but no region starts here */
    minefield_admin[start + 1] = region_start % PAGESIZE;

    minefield_admin_hide(1);

    VirtualAlloc((char *) minefield_pages + region_start, size,
		 MEM_COMMIT, PAGE_READWRITE);
    return (char *) minefield_pages + region_start;
}

static void minefield_free(void *ptr)
{
    int region_start, i, j;

    minefield_admin_hide(0);

    region_start = (char *) ptr - (char *) minefield_pages;
    i = region_start / PAGESIZE;
    if (i < 0 || i >= minefield_npages ||
	minefield_admin[i] != region_start % PAGESIZE)
	minefield_bomb();
    for (j = i; j < minefield_npages && minefield_admin[j] != 0xFFFF; j++) {
	minefield_admin[j] = 0xFFFF;
    }

    VirtualFree(ptr, j * PAGESIZE - region_start, MEM_DECOMMIT);

    minefield_admin_hide(1);
}

static int minefield_get_size(void *ptr)
{
    int region_start, i, j;

    minefield_admin_hide(0);

    region_start = (char *) ptr - (char *) minefield_pages;
    i = region_start / PAGESIZE;
    if (i < 0 || i >= minefield_npages ||
	minefield_admin[i] != region_start % PAGESIZE)
	minefield_bomb();
    for (j = i; j < minefield_npages && minefield_admin[j] != 0xFFFF; j++);

    minefield_admin_hide(1);

    return j * PAGESIZE - region_start;
}

void *minefield_c_malloc(size_t size)
{
    if (!minefield_initialised)
	minefield_init();
    return minefield_alloc(size);
}

void minefield_c_free(void *p)
{
    if (!minefield_initialised)
	minefield_init();
    minefield_free(p);
}

/*
 * realloc _always_ moves the chunk, for rapid detection of code
 * that assumes it won't.
 */
void *minefield_c_realloc(void *p, size_t size)
{
    size_t oldsize;
    void *q;
    if (!minefield_initialised)
	minefield_init();
    q = minefield_alloc(size);
    oldsize = minefield_get_size(p);
    memcpy(q, p, (oldsize < size ? oldsize : size));
    minefield_free(p);
    return q;
}

#endif				/* MINEFIELD */

FontSpec *fontspec_new(const char *name,
                        int bold, int height, int charset)
{
    FontSpec *f = snew(FontSpec);
    f->name = dupstr(name);
    f->isbold = bold;
    f->height = height;
    f->charset = charset;
    return f;
}
FontSpec *fontspec_copy(const FontSpec *f)
{
    return fontspec_new(f->name, f->isbold, f->height, f->charset);
}
void fontspec_free(FontSpec *f)
{
    sfree(f->name);
    sfree(f);
}
void fontspec_serialise(BinarySink *bs, FontSpec *f)
{
    put_asciz(bs, f->name);
    put_uint32(bs, f->isbold);
    put_uint32(bs, f->height);
    put_uint32(bs, f->charset);
}
FontSpec *fontspec_deserialise(BinarySource *src)
{
    const char *name = get_asciz(src);
    unsigned isbold = get_uint32(src);
    unsigned height = get_uint32(src);
    unsigned charset = get_uint32(src);
    return fontspec_new(name, isbold, height, charset);
}

int open_for_write_would_lose_data(const Filename *fn)
{
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesEx(fn->path, GetFileExInfoStandard, &attrs)) {
        /*
         * Generally, if we don't identify a specific reason why we
         * should return true from this function, we return false, and
         * let the subsequent attempt to open the file for real give a
         * more useful error message.
         */
        return FALSE;
    }
    if (attrs.dwFileAttributes & (FILE_ATTRIBUTE_DEVICE |
                                  FILE_ATTRIBUTE_DIRECTORY)) {
        /*
         * File is something other than an ordinary disk file, so
         * opening it for writing will not cause truncation. (It may
         * not _succeed_ either, but that's not our problem here!)
         */
        return FALSE;
    }
    if (attrs.nFileSizeHigh == 0 && attrs.nFileSizeLow == 0) {
        /*
         * File is zero-length (or may be a named pipe, which
         * dwFileAttributes can't tell apart from a regular file), so
         * opening it for writing won't truncate any data away because
         * there's nothing to truncate anyway.
         */
        return FALSE;
    }
    return TRUE;
}
