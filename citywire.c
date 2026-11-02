/* carhacks/citywire.c - how a car's CITY is spelled on every MP wire.
 *
 * Stock rows 0..10 travel as their index. A runtime bank (row 11+) is spelled by
 * a code from its NAME (jer_carquery.h, JER_CARQ_CITYWIRE), because its row number
 * is just "whichever row was free on this machine". Each machine also tells the
 * session which names it uses for which codes (CHK_NET_NAMES); when two different
 * names share a code anywhere in the session, that code is AMBIGUOUS and is read as
 * "not held here" - a stand-in, never the wrong car. */

#include "driver2.h"
#include "system.h"
#include "jericho.h"
#include "jer_carquery.h"

#include "carid.h"
#include "net.h"

#include <string.h>
#include <ctype.h>

#define CW_CODES	(JER_CITY_WIRE_CODE_LAST - JER_CITY_WIRE_CODE_FIRST + 1)	/* 127 */
#define CW_NAME_MAX	32

static char gCwHeard[CW_CODES][CW_NAME_MAX];	/* a name some machine uses for this code */
static unsigned char gCwAmbiguous[CW_CODES];

int chkCityWireCodeForName(const char* name)
{
	unsigned h = 2166136261u;

	for (; *name; name++)
	{
		h ^= (unsigned)toupper((unsigned char)*name);
		h *= 16777619u;
	}
	return JER_CITY_WIRE_CODE_FIRST + (int)(h % CW_CODES);
}

static int CwSameName(const char* a, const char* b)
{
	for (; *a && *b; a++, b++)
		if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
			return 0;
	return *a == *b;
}

/* Record that some machine (this one included) spells `name` as `code`. */
static void CwNote(int code, const char* name)
{
	int i = code - JER_CITY_WIRE_CODE_FIRST;

	if (i < 0 || i >= CW_CODES || name[0] == 0)
		return;
	if (gCwHeard[i][0] == 0)
	{
		strncpy(gCwHeard[i], name, CW_NAME_MAX - 1);
		return;
	}
	if (!gCwAmbiguous[i] && !CwSameName(gCwHeard[i], name))
	{
		gCwAmbiguous[i] = 1;
		printInfo("[carhacks/net] city wire code 0x%02X is used by both %s and %s in this session - "
			"cars from either show as stand-ins until one is renamed\n", code, gCwHeard[i], name);
	}
}

static void CwNoteLocal(void)
{
	int city;

	for (city = JerCityFirstCustom(); city < CITY_COUNT; city++)
		if (JerCityRegistered(city))
			CwNote(chkCityWireCodeForName(JerCityName(city)), JerCityName(city));
}

int chkCityToWire(int city)
{
	if (city < 0 || city == CHK_CITY_NATIVE)
		return CHK_CITY_NATIVE;
	if (city >= JER_CITY_WIRE_CODE_FIRST && city <= JER_CITY_WIRE_CODE_LAST)
		return city;			/* an opaque code we were handed: forward it as-is */
	if (city < JerCityFirstCustom())
		return city;			/* stock: the index is the same everywhere */
	if (city < CITY_COUNT && JerCityRegistered(city))
		return chkCityWireCodeForName(JerCityName(city));
	return JER_CITY_WIRE_UNSPELLABLE;
}

int chkCityFromWire(int wire)
{
	int city, i;

	if (wire == CHK_CITY_NATIVE)
		return CHK_CITY_NATIVE;
	if (wire >= 0 && wire < JerCityFirstCustom())
		return wire;
	if (wire < JER_CITY_WIRE_CODE_FIRST || wire > JER_CITY_WIRE_CODE_LAST)
		return JER_CITY_WIRE_UNSPELLABLE;	/* includes raw rows 11..: never trust one */

	CwNoteLocal();
	i = wire - JER_CITY_WIRE_CODE_FIRST;
	if (gCwAmbiguous[i])
		return wire;

	for (city = JerCityFirstCustom(); city < CITY_COUNT; city++)
		if (JerCityRegistered(city) && chkCityWireCodeForName(JerCityName(city)) == wire)
			return city;

	return wire;				/* not held here: opaque, unusable as a row */
}

/* v1 peers spell every city by raw index: only the stock rows mean the same here. */
int chkCityFromWireV1(int raw)
{
	if (raw == CHK_CITY_NATIVE || (raw >= 0 && raw < JerCityFirstCustom()))
		return raw;
	return JER_CITY_WIRE_UNSPELLABLE;
}

/* CHK_NET_NAMES payload: [count] then count x [code][len][name bytes]. */
int chkCityWireBuildNames(unsigned char* out, int max)
{
	int city, len = 1, count = 0;

	for (city = JerCityFirstCustom(); city < CITY_COUNT; city++)
	{
		const char* name;
		int n;

		if (!JerCityRegistered(city))
			continue;
		name = JerCityName(city);
		n = (int)strlen(name);
		if (n >= CW_NAME_MAX)
			n = CW_NAME_MAX - 1;
		if (len + 2 + n > max)
			break;
		out[len++] = (unsigned char)chkCityWireCodeForName(name);
		out[len++] = (unsigned char)n;
		memcpy(out + len, name, (size_t)n);
		len += n;
		count++;
	}
	out[0] = (unsigned char)count;
	return count ? len : 0;
}

void chkCityWireOnNames(const unsigned char* p, int len)
{
	int at = 1, i, count;

	if (len < 1)
		return;
	CwNoteLocal();
	count = p[0];
	for (i = 0; i < count && at + 2 <= len; i++)
	{
		char name[CW_NAME_MAX];
		int code = p[at], n = p[at + 1];

		at += 2;
		if (n >= CW_NAME_MAX || at + n > len)
			break;
		memcpy(name, p + at, (size_t)n);
		name[n] = 0;
		at += n;
		CwNote(code, name);
	}
}

void chkCityWireReset(void)
{
	memset(gCwHeard, 0, sizeof(gCwHeard));
	memset(gCwAmbiguous, 0, sizeof(gCwAmbiguous));
}

/* JER_CARQ_CITYWIRE: mp asks how to spell / read a city on its own wire. */
static int ChkOnCityWire(void* ud, void* args)
{
	JER_CARQ_CITYWIRE_ARGS* a = (JER_CARQ_CITYWIRE_ARGS*)args;
	int v;

	(void)ud;
	if (a == NULL)
		return JER_RESULT_CONTINUE;

	if (a->toWire)
		a->wire = chkCityToWire(a->city);
	else
	{
		v = chkCityFromWire(a->wire);
		a->city = (v == CHK_CITY_NATIVE) ? -1 : v;
	}
	a->handled = 1;
	return JER_RESULT_CONTINUE;
}

void chkCityWireRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_CARQ_CITYWIRE, ChkOnCityWire, NULL, 0);
}
