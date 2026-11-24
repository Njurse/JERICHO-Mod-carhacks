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
 *   - which mods: every folder under MODS except one switched off ("<id> = 0" in
 *                JERICHO/CONFIG/modlist.ini, what the store and the Mods menu write). A
 *                mod with NO line is scanned: a bank built locally (rebake, a Driver 1 /
 *                Euro-43 conversion copied in) never went through the store.
 *   - where:     MODS/<id>/banks/<dir>/, then MODS/<id>/ itself, then one folder below
 *                it (vehicles/). build/, source/, donor/, backups, tools/ and the bake's
 *                art folders are never read: they hold copies, not the bank to offer.
 *   - what:      a folder with bank.json is that bank (its "city", <CITY>.LEV or the
 *                json's "lev"; colours optional). Without one, every <NAME>.LEV that has
 *                a <NAME>.LCF beside it is a converted bank named NAME (the LCF is what
 *                marks a car bank - a pedestrian LEV has none).
 *   - a city name already registered by something else (a stock row, Beta Miami,
 *     a cities.ini row) is left alone and logged: first wins, cities.ini coexists.
 *     That is also what keeps a HAVANA re-export from shadowing the stock city.
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

/* JERICHO: "<id> = 0" in modlist.ini. A mod the list does not mention is NOT off: a bank
 * built locally (rebake, a Driver 1 or Euro-43 conversion dropped into MODS) never goes
 * through the store, so it has no line - and it is exactly the bank that used to need a
 * hand-written cities.ini row. An explicit 0 (the store's and the Mods menu's "off")
 * still wins. */
static int PbModDisabled(const PB_NAME* off, int nOff, const char* mod)
{
	int i;

	for (i = 0; i < nOff; i++)
		if (!strcmp(off[i], mod))
			return 1;
	return 0;
}

static int PbDisabledMods(const char* root, PB_NAME* out, int max)
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
		char* id = PbTrim(line);
		char* val;

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

		if (id[0] && strlen(id) < PB_NAME_MAX && !PbTruthy(val) && n < max)
			strcpy(out[n++], id);
	}
	fclose(f);
	return n;
}

/* Folders a bank is never taken from: what the bake writes beside the real bank
 * (build/), the art and copies it keeps (source/, backups, donor/) and tooling. A bank's
 * own LEV/LCF sit beside its bank.json, so none of these hold the one to offer. */
static int PbSkipDir(const char* name)
{
	static const char* skip[] = { "build", "source", "donor", "tools", "out", "work",
		"cars", "shared", "textures", "sounds", "pages", "models", "lib", "docs", "tmp",
		"reference_material", "police", NULL };
	char low[PB_NAME_MAX];
	int i;

	for (i = 0; name[i] && i < PB_NAME_MAX - 1; i++)
		low[i] = (char)tolower((unsigned char)name[i]);
	low[i] = 0;

	if (low[0] == '_' || strstr(low, "backup") != NULL || strstr(low, ".portraits") != NULL)
		return 1;

	for (i = 0; skip[i]; i++)
		if (!strcmp(low, skip[i]))
			return 1;
	return 0;
}

/* `<rel>` is the bank folder under MODS/<mod> ("" for the mod's own folder), '/'-joined. */
static void PbDiskPath(char* out, int size, const char* root, const char* mod, const char* rel, const char* file)
{
	if (rel[0])
		snprintf(out, size, "%s/MODS/%s/%s/%s", root, mod, rel, file);
	else
		snprintf(out, size, "%s/MODS/%s/%s", root, mod, file);
}

/* The DRIVER2-relative spelling a city row stores: ..\JERICHO\MODS\<mod>\<rel>\<file>. */
static void PbRelPath(char* out, int size, const char* mod, const char* rel, const char* file)
{
	char* c;

	if (rel[0])
		snprintf(out, size, "..\\JERICHO\\MODS\\%s\\%s\\%s", mod, rel, file);
	else
		snprintf(out, size, "..\\JERICHO\\MODS\\%s\\%s", mod, file);

	for (c = out; *c; c++)
		if (*c == '/')
			*c = '\\';
}

/* Pick the bank's file: <rel>/<CITY>.<ext> first (how the store zip ships it), then
 * bank.json's own relative entry. Writes the DRIVER2-relative spelling. */
static int PbResolve(const char* root, const char* mod, const char* rel, const char* city,
	const char* json, const char* key, const char* ext, char* out, int outSize)
{
	char disk[PB_PATH_MAX], entry[PB_PATH_MAX], file[PB_NAME_MAX + 8];

	snprintf(file, sizeof(file), "%s.%s", city, ext);
	PbDiskPath(disk, sizeof(disk), root, mod, rel, file);
	if (PbFileExists(disk))
	{
		PbRelPath(out, outSize, mod, rel, file);
		return 1;
	}

	if (json != NULL && PbJsonString(json, key, entry, sizeof(entry)) && strstr(entry, "..") == NULL)
	{
		PbDiskPath(disk, sizeof(disk), root, mod, rel, entry);
		if (PbFileExists(disk))
		{
			PbRelPath(out, outSize, mod, rel, entry);
			return 1;
		}
	}

	return 0;
}

static int PbIEq(const char* a, const char* b)
{
	while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
		a++, b++;
	return *a == 0 && *b == 0;
}

/* The file in `files` whose name is `stem` + "." + `ext`, any letter case, or -1. */
static int PbFindExt(PB_NAME* files, int n, const char* stem, const char* ext)
{
	int i, sl = (int)strlen(stem);

	for (i = 0; i < n; i++)
	{
		const char* f = files[i];

		if ((int)strlen(f) != sl + 1 + (int)strlen(ext) || f[sl] != '.')
			continue;
		if (strncmp(f, stem, sl) != 0)
			continue;
		if (PbIEq(f + sl + 1, ext))
			return i;
	}
	return -1;
}

/* Offer one bank. Shared by the bank.json and the bare LEV+LCF paths. */
static void PbOffer(const char* city, const char* mod, const char* lev, const char* lcf, char* seen)
{
	int own = PbOwnedIndex(city), row;

	if (own >= 0 && strcmp(gPbOwned[own].mod, mod) != 0)
	{
		if (PbFirstLog(city, mod))
			printInfo("[carhacks] pack bank %s in %s skipped: %s already registered it\n", city, mod, gPbOwned[own].mod);
		return;
	}
	if (own >= 0 && seen[own])
		return;
	if (PbTakenByOther(city))
	{
		if (PbFirstLog(city, mod))
			printInfo("[carhacks] pack bank %s in %s left alone: that city is already registered (stock, Beta Miami or cities.ini)\n", city, mod);
		return;
	}

	row = JerCityRegister(city, lev, (lcf != NULL && lcf[0]) ? lcf : NULL);
	if (row < 0)
	{
		if (PbFirstLog(city, mod))
			printInfo("[carhacks] pack bank %s in %s not registered (no free bank row or file missing)\n", city, mod);
		return;
	}

	if (own < 0 && gPbOwnedCount < CITY_CUSTOM_MAX)
	{
		own = gPbOwnedCount++;
		snprintf(gPbOwned[own].city, PB_NAME_MAX, "%s", city);
		snprintf(gPbOwned[own].mod, PB_NAME_MAX, "%s", mod);
		printInfo("[carhacks] bank %s found in %s - registered as city %d (%s)\n", city, mod, row, lev);
	}
	if (own >= 0)
		seen[own] = 1;
}

/* One candidate folder: a bank.json names its city; without one, every <NAME>.LEV that
 * has a <NAME>.LCF beside it is a converted bank (the LCF is what tells a car bank from
 * a pedestrian or level file, which carry no car colours). */
static void PbScanDir(const char* root, const char* mod, const char* rel, char* seen)
{
	static char json[8192];
	static PB_NAME files[PB_MAX_NAMES];
	char path[PB_PATH_MAX], city[PB_NAME_MAX], lev[PB_PATH_MAX], lcf[PB_PATH_MAX];
	int n, i;

	PbDiskPath(path, sizeof(path), root, mod, rel, "bank.json");

	if (PbReadFile(path, json, sizeof(json)) && PbJsonString(json, "city", city, sizeof(city)))
	{
		if (!PbResolve(root, mod, rel, city, json, "lev", "LEV", lev, sizeof(lev)))
			return;		/* declared but not baked yet */
		if (!PbResolve(root, mod, rel, city, json, "lcf", "LCF", lcf, sizeof(lcf)))
			lcf[0] = 0;
		PbOffer(city, mod, lev, lcf, seen);
		return;
	}

	PbDiskPath(path, sizeof(path), root, mod, rel, "");
	n = PbList(path, files, PB_MAX_NAMES, 1);

	for (i = 0; i < n; i++)
	{
		char stem[PB_NAME_MAX];
		int len = (int)strlen(files[i]), k, c;

		if (len < 5 || files[i][len - 4] != '.')
			continue;
		if (!PbIEq(files[i] + len - 3, "LEV"))
			continue;

		snprintf(stem, sizeof(stem), "%.*s", len - 4, files[i]);
		c = PbFindExt(files, n, stem, "LCF");
		if (c < 0)
			continue;

		PbRelPath(lev, sizeof(lev), mod, rel, files[i]);
		PbRelPath(lcf, sizeof(lcf), mod, rel, files[c]);

		/* a city row's name is upper case, like the stock ones */
		for (k = 0; stem[k]; k++)
			city[k] = (char)toupper((unsigned char)stem[k]);
		city[k] = 0;

		PbOffer(city, mod, lev, lcf, seen);
	}
}

void chkPackBanksRefresh(void)
{
	static PB_NAME mods[PB_MAX_NAMES];
	static PB_NAME off[PB_MAX_NAMES];
	static PB_NAME dirs[PB_MAX_NAMES];
	static char seen[CITY_CUSTOM_MAX];
	const char* root = jer_root_dir();
	char path[PB_PATH_MAX];
	int nMods, nOff, m, i;

	if (root == NULL || root[0] == 0)
		return;

	memset(seen, 0, sizeof(seen));

	/* Every mod folder (sorted, so the same set lands on the same rows on every machine),
	 * minus the ones switched off. */
	snprintf(path, sizeof(path), "%s/MODS", root);
	nMods = PbList(path, mods, PB_MAX_NAMES, 0);
	nOff = PbDisabledMods(root, off, PB_MAX_NAMES);

	for (m = 0; m < nMods; m++)
	{
		int nDirs, d;

		if (PbModDisabled(off, nOff, mods[m]))
			continue;

		/* 1. banks/<dir>/ - the layout BAKE and the store both use */
		snprintf(path, sizeof(path), "%s/MODS/%s/banks", root, mods[m]);
		nDirs = PbList(path, dirs, PB_MAX_NAMES, 0);

		for (d = 0; d < nDirs; d++)
		{
			char rel[PB_PATH_MAX];

			if (PbSkipDir(dirs[d]))
				continue;
			snprintf(rel, sizeof(rel), "banks/%s", dirs[d]);
			PbScanDir(root, mods[m], rel, seen);
		}

		/* 2. the mod's own folder (DEMOCARS.LEV beside mod.toml) and 3. one level below
		 * it (vehicles/CROSSFIRE.LEV). A name already taken above, or by a stock city
		 * (a HAVANA re-export), is left alone by PbOffer. */
		PbScanDir(root, mods[m], "", seen);

		snprintf(path, sizeof(path), "%s/MODS/%s", root, mods[m]);
		nDirs = PbList(path, dirs, PB_MAX_NAMES, 0);

		for (d = 0; d < nDirs; d++)
		{
			if (!strcmp(dirs[d], "banks") || PbSkipDir(dirs[d]))
				continue;
			PbScanDir(root, mods[m], dirs[d], seen);
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
