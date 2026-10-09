/*
 * SetToolType <icon> <NAME=VALUE> - set (or add) one tooltype of an icon.
 *
 * <icon> is given without ".info". An existing NAME or NAME=... entry is
 * replaced; otherwise the entry is appended. Used to point a Picasso96
 * monitor file at MiSTerZZ.card (BOARDTYPE=MiSTerZZ) from the CLI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <proto/exec.h>
#include <proto/icon.h>
#include <workbench/workbench.h>
#include <stdio.h>
#include <string.h>

struct Library *IconBase;

static int same_key(const char *a, const char *b)
{
	while (*a && *a != '=' && *b && *b != '=') {
		char x = *a++, y = *b++;
		if (x >= 'a' && x <= 'z') x -= 32;
		if (y >= 'a' && y <= 'z') y -= 32;
		if (x != y) return 0;
	}
	return (*a == 0 || *a == '=') && (*b == 0 || *b == '=');
}

int main(int argc, char **argv)
{
	static STRPTR tt[64];
	struct DiskObject *dob;
	int n = 0, found = 0, rc = 0;

	if (argc != 3) {
		printf("usage: SetToolType <icon> <NAME=VALUE>\n");
		return 10;
	}
	if (!(IconBase = OpenLibrary((STRPTR)"icon.library", 36)))
		return 20;
	if (!(dob = GetDiskObject((STRPTR)argv[1]))) {
		printf("SetToolType: cannot read %s.info\n", argv[1]);
		CloseLibrary(IconBase);
		return 10;
	}

	for (STRPTR *p = (STRPTR *)dob->do_ToolTypes; p && *p && n < 62; p++) {
		if (same_key((char *)*p, argv[2])) {
			tt[n++] = (STRPTR)argv[2];
			found = 1;
		} else {
			tt[n++] = *p;
		}
	}
	if (!found)
		tt[n++] = (STRPTR)argv[2];
	tt[n] = NULL;

	STRPTR *old = (STRPTR *)dob->do_ToolTypes;
	dob->do_ToolTypes = tt;
	if (!PutDiskObject((STRPTR)argv[1], dob)) {
		printf("SetToolType: cannot write %s.info\n", argv[1]);
		rc = 10;
	} else {
		printf("%s: %s %s\n", argv[1], found ? "replaced" : "added", argv[2]);
	}
	dob->do_ToolTypes = old;
	FreeDiskObject(dob);
	CloseLibrary(IconBase);
	return rc;
}
