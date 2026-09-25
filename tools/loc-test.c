/*
 * loc-test.c -- the localization file parser (WinQuake/localize.c), on its own.
 *
 * Built and run by smoke-test.sh:
 *
 *     cc -IWinQuake -o loc-test tools/loc-test.c -lz && ./loc-test
 *
 * It includes localize.c whole so it can reach the parser, which is static,
 * and stands in for the few engine functions the file calls. Each line of the
 * text below is how a key appears in the re-release's loc_english.txt, and
 * each check is what the progs should get back for it.
 */

#include "localize.c"

char	com_basedir[MAX_OSPATH];

void Con_Printf (char *fmt, ...) { (void)fmt; }
int Q_strncasecmp (char *a, char *b, int n) { return strncasecmp (a, b, n); }
void Q_strncpy (char *d, char *s, int n) { strncpy (d, s, n); }
void COM_ForEachFile (char *name, void (*fn)(char *, char *))
{
	(void)name; (void)fn;
}

static int check (const char *key, const char *want)
{
	const char	*got = LOC_GetString (key);
	int			ok = !strcmp (got, want);

	printf ("[loc-test] %s %s -> \"%s\"", ok ? "ok  " : "FAIL", key, got);
	if (!ok)
		printf (", expected \"%s\"", want);
	printf ("\n");
	return ok;
}

int main (void)
{
	static char	text[] =
		"// a comment\n"
		"qc_backpack_got = \"You got \"\n"		// the one that lost its space
		"$qc_backpack_cells = \"{0} cells\"\n"
		"qc_escaped = \"a\\\"b \"\n"
		"qc_newline = \"line\\n\"\n"
		"qc_unquoted = trailing   \r\n"
		"qc_padded = \"  both  \"\r\n";
	int			ok = 1;

	LOC_AddText (text, "loc-test");
	LOC_BuildIndex ();

	ok &= check ("$qc_backpack_got", "You got ");
	ok &= check ("$qc_backpack_cells", "{0} cells");
	ok &= check ("$qc_escaped", "a\"b ");
	ok &= check ("$qc_newline", "line\n");
	ok &= check ("$qc_unquoted", "trailing");
	ok &= check ("$qc_padded", "  both  ");
	ok &= check ("not a key", "not a key");

	return !ok;
}
