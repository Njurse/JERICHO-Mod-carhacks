/* carhacks/packbanks.c - auto-register car packs (data-only mods) as city banks.
 *
 * A car pack is a store mod with no DLL whose unpacked folder carries one or
 * more BAKE banks:  JERICHO/MODS/<id>/banks/<dir>/bank.json  (+ <CITY>.LEV/.LCF).
 * The loader never runs a data mod, so before this nothing registered them; a
 * player had to paste a cities.ini row. Now, beside ChkRegisterBetaBank, every
 * enabled mod's banks are registered through JerCityRegister - the same
 * module-owned path Beta Miami uses - into the custom rows (JerCityFirstCustom()
 * and up). Nothing here touches the stock rows, carNumLookup for the stock
 * cities, or the reserved Chicago truck slot.
 *
 * Rules (zero pack-format change; HallgarthCars 0.1.1 works as published):
 *   - enabled  = `<id> = 1` in JERICHO/CONFIG/modlist.ini (what the store and the
 *                Mods menu write). Absent means off: a data pack is opt-in.
 *   - level    = <bankdir>/<CITY>.LEV, else bank.json's "lev" relative to the bank.
 *     colours  = the same for .LCF / "lcf"; optional.
 *   - a city name already registered by something else (a stock row, Beta Miami,
 *     a cities.ini row) is left alone and logged: first wins, cities.ini coexists.
 *   - mods and banks are walked in sorted order, so the same pack set lands on the
 *     same rows on every machine.
 *   - a bank this file registered whose mod was disabled or whose files went is
 *     unregistered (JerCityRegister(name, NULL, NULL)).
 *
 * The cars a bank offers come from its own LUMP_CAR_MODELS, in model order
 * (chkFillCarTableFromImport); BAKE writes that from declare.order, so the menu's
 * CAR 1, CAR 2 follow bank.json. */

#include "driver2.h"
#include "system.h"
#include "jericho.h"
#include "carimport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define PB_MAX_NAMES	64
#define PB_NAME_MAX	64
#define PB_PATH_MAX	768

typedef struct PB_OWNED
{
	char city[PB_NAME_MAX];
	char mod[PB_NAME_MAX];
} PB_OWNED;

static PB_OWNED gPbOwned[CITY_CUSTOM_MAX];
static int gPbOwnedCount;

typedef char PB_NAME[PB_NAME_MAX];

static int PbCmp(const void* a, const void* b)
{
	return strcmp((const char*)a, (const char*)b);
}

/* Sorted subdirectory names of `path` (or regular files when `files`). */
static int PbList(const char* path, PB_NAME* out, int max, int files)
{
	int n = 0;
#if defined(_WIN32)
	char pattern[PB_PATH_MAX];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	snprintf(pattern, sizeof(pattern), "%s\\*", path);
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		int isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

		if (fd.cFileName[0] == '.' || isDir == files || strlen(fd.cFileName) >= PB_NAME_MAX)
			continue;
		if (n < max)
			strcpy(out[n++], fd.cFileName);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR* d = opendir(path);
	struct dirent* e;

	if (d == NULL)
		return 0;
	while ((e = readdir(d)) != NULL)
	{
		char full[PB_PATH_MAX];
		struct stat st;

		if (e->d_name[0] == '.' || strlen(e->d_name) >= PB_NAME_MAX)
			continue;
		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		if (stat(full, &st) != 0 || (S_ISDIR(st.st_mode) ? 1 : 0) == files)
			continue;
		if (n < max)
			strcpy(out[n++], e->d_name);
	}
	closedir(d);
#endif
	qsort(out, n, sizeof(PB_NAME), PbCmp);
	return n;
}

static int PbFileExists(const char* path)
{
	FILE* f = fopen(path, "rb");

	if (f == NULL)
		return 0;
	fclose(f);
	return 1;
}

static char* PbTrim(char* s)
{
	char* e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (unsigned char)e[-1] <= ' ')
		*--e = 0;
	return s;
}

static int PbTruthy(const char* v)
{
	return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "enabled") || !strcmp(v, "yes");
}

/* Enabled mod ids from modlist.ini, sorted. */
static int PbEnabledMods(const char* root, PB_NAME* out, int max)
{
	char path[PB_PATH_MAX], line[256];
	FILE* f;
	int n = 0;

	snprintf(path, sizeof(path), "%s/CONFIG/modlist.ini", root);
	f = fopen(path, "r");
	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL)
	{
		char* eq;
		char* id;
		char* val;

		id = PbTrim(line);
		if (id[0] == '#' || id[0] == ';' || id[0] == '[')
			continue;
		eq = strchr(id, '=');
		if (eq == NULL)
			continue;
		*eq = 0;
		id = PbTrim(id);
		val = PbTrim(eq + 1);
		for (char* c = val; *c; c++)
			*c = (char)tolower((unsigned char)*c);

		if (id[0] && strlen(id) < PB_NAME_MAX && PbTruthy(val) && n < max)
			strcpy(out[n++], id);
	}
	fclose(f);

	qsort(out, n, sizeof(PB_NAME), PbCmp);
	return n;
}

/* The string value of a top-level "key": "value" in a small JSON file. Good
 * enough for bank.json, which BAKE writes; answers 0 when it is not there. */
static int PbJsonString(const char* json, const char* key, char* out, int outSize)
{
	char pat[64];
	const char* p;
	int n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (p == NULL)
		return 0;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
		p++;
	if (*p++ != '"')
		return 0;
	while (*p && *p != '"' && n < outSize - 1)
		out[n++] = *p++;
	out[n] = 0;
	return n > 0;
}

static int PbReadFile(const char* path, char* buf, int size)
{
	FILE* f = fopen(path, "rb");
	int n;

	if (f == NULL)
		return 0;
	n = (int)fread(buf, 1, size - 1, f);
	fclose(f);
	buf[n > 0 ? n : 0] = 0;
	return n > 0;
}

/* Log a skip once per (city, mod), not on every car-screen refresh. */
static int PbFirstLog(const char* city, const char* mod)
{
	static char logged[PB_MAX_NAMES][PB_NAME_MAX * 2];
	char key[PB_NAME_MAX * 2];
	int k;

	snprintf(key, sizeof(key), "%s/%s", mod, city);
	for (k = 0; k < PB_MAX_NAMES && logged[k][0]; k++)
		if (!strcmp(logged[k], key))
			return 0;
	if (k < PB_MAX_NAMES)
		strcpy(logged[k], key);
	return 1;
}

static int PbOwnedIndex(const char* city)
{
	int i;

	for (i = 0; i < gPbOwnedCount; i++)
		if (!strcmp(gPbOwned[i].city, city))
			return i;
	return -1;
}

/* Is `city` a registered row that this file does not own? */
static int PbTakenByOther(const char* city)
{
	int i;

	if (PbOwnedIndex(city) >= 0)
		return 0;
	for (i = 0; i < CITY_COUNT; i++)
		if (JerCityRegistered(i) && !strcmp(JerCityName(i), city))
			return 1;
	return 0;
}

/* Pick the bank's file: <dir>/<CITY>.<ext> first (how the store zip ships it),
 * then bank.json's own relative entry. Writes the DRIVER2-relative spelling. */
static int PbResolve(const char* root, const char* mod, const char* dir, const char* city,
	const char* json, const char* key, const char* ext, char* rel, int relSize)
{
	char disk[PB_PATH_MAX], entry[PB_PATH_MAX];

	snprintf(disk, sizeof(disk), "%s/MODS/%s/banks/%s/%s.%s", root, mod, dir, city, ext);
	if (PbFileExists(disk))
	{
		snprintf(rel, relSize, "..\\JERICHO\\MODS\\%s\\banks\\%s\\%s.%s", mod, dir, city, ext);
		return 1;
	}

	if (PbJsonString(json, key, entry, sizeof(entry)) && strstr(entry, "..") == NULL)
	{
		char* c;

		snprintf(disk, sizeof(disk), "%s/MODS/%s/banks/%s/%s", root, mod, dir, entry);
		if (PbFileExists(disk))
		{
			for (c = entry; *c; c++)
				if (*c == '/')
					*c = '\\';
			snprintf(rel, relSize, "..\\JERICHO\\MODS\\%s\\banks\\%s\\%s", mod, dir, entry);
			return 1;
		}
	}

	return 0;
}

void chkPackBanksRefresh(void)
{
	static PB_NAME mods[PB_MAX_NAMES];
	static PB_NAME dirs[PB_MAX_NAMES];
	static char seen[CITY_CUSTOM_MAX];
	static char json[8192];
	const char* root = jer_root_dir();
	int nMods, m, i;

	if (root == NULL || root[0] == 0)
		return;

	memset(seen, 0, sizeof(seen));
	nMods = PbEnabledMods(root, mods, PB_MAX_NAMES);

	for (m = 0; m < nMods; m++)
	{
		char banks[PB_PATH_MAX];
		int nDirs, d;

		snprintf(banks, sizeof(banks), "%s/MODS/%s/banks", root, mods[m]);
		nDirs = PbList(banks, dirs, PB_MAX_NAMES, 0);

		for (d = 0; d < nDirs; d++)
		{
			char path[PB_PATH_MAX], city[PB_NAME_MAX], lev[PB_PATH_MAX], lcf[PB_PATH_MAX];
			int own, row;

			snprintf(path, sizeof(path), "%s/%s/bank.json", banks, dirs[d]);
			if (!PbReadFile(path, json, sizeof(json)) || !PbJsonString(json, "city", city, sizeof(city)))
				continue;

			own = PbOwnedIndex(city);
			if (own >= 0 && strcmp(gPbOwned[own].mod, mods[m]) != 0)
			{
				if (PbFirstLog(city, mods[m]))
					printInfo("[carhacks] pack bank %s in %s skipped: %s already registered it\n", city, mods[m], gPbOwned[own].mod);
				continue;
			}
			if (own >= 0 && seen[own])
				continue;
			if (PbTakenByOther(city))
			{
				if (PbFirstLog(city, mods[m]))
					printInfo("[carhacks] pack bank %s in %s left alone: that city is already registered (stock, Beta Miami or cities.ini)\n", city, mods[m]);
				continue;
			}
			if (!PbResolve(root, mods[m], dirs[d], city, json, "lev", "LEV", lev, sizeof(lev)))
				continue;
			if (!PbResolve(root, mods[m], dirs[d], city, json, "lcf", "LCF", lcf, sizeof(lcf)))
				lcf[0] = 0;

			row = JerCityRegister(city, lev, lcf[0] ? lcf : NULL);
			if (row < 0)
			{
				printInfo("[carhacks] pack bank %s in %s not registered (no free bank row or file missing)\n", city, mods[m]);
				continue;
			}

			if (own < 0 && gPbOwnedCount < CITY_CUSTOM_MAX)
			{
				own = gPbOwnedCount++;
				snprintf(gPbOwned[own].city, PB_NAME_MAX, "%s", city);
				snprintf(gPbOwned[own].mod, PB_NAME_MAX, "%s", mods[m]);
				printInfo("[carhacks] pack bank %s registered as city %d from %s\n", city, row, mods[m]);
			}
			if (own >= 0)
				seen[own] = 1;
		}
	}

	/* What we registered last time and did not see now: the mod was disabled or
	 * its files went. Free the row. */
	for (i = gPbOwnedCount - 1; i >= 0; i--)
	{
		if (seen[i])
			continue;
		printInfo("[carhacks] pack bank %s from %s unregistered (mod disabled or files gone)\n", gPbOwned[i].city, gPbOwned[i].mod);
		JerCityRegister(gPbOwned[i].city, NULL, NULL);
		gPbOwned[i] = gPbOwned[--gPbOwnedCount];
		seen[i] = seen[gPbOwnedCount];
	}
}
