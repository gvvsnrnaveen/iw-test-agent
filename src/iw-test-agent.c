/*
 * iw-test-agent - DUT side agent for the iw-test-agent Wi-Fi test controller.
 *
 * Connects to the laptop controller over TCP, receives commands as
 * newline-delimited flat JSON objects and executes them on an OpenWrt
 * based DUT (uci / wifi / iw / ping).  Only libc is required so it can be
 * cross-compiled with the OpenWrt SDK.
 *
 * Wire protocol (one JSON object per line, flat: no nested objects):
 *   agent -> ctrl : {"type":"hello","name":..,"token":..,"version":..}
 *   ctrl  -> agent: {"type":"cmd","id":N,"op":"<op>", ...params}
 *   agent -> ctrl : {"type":"result","id":N,"ok":true|false,"error":.., ...data}
 *
 * Ops: info, prepare, apply, wait_link, status, ping, teardown, restore, noop
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AGENT_VERSION   "1.0.0"
#define MAX_KV          64
#define RX_BUF_LEN      16384
#define OUT_MAX         65536
#define RES_MAX         32768
#define MAX_SECTIONS    32
#define MAX_PHYS        8
#define TEST_IFNAME     "fpt0"
#define TEST_NET        "fpt"
#define BACKUP_DIR      "/tmp/iw_test_agent_backup"
#define BACKUP_MARKER   BACKUP_DIR "/.active"

/* ------------------------------------------------------------------ */
/* globals / options                                                    */
/* ------------------------------------------------------------------ */

static const char *g_server = NULL;
static int g_port = 5555;
static char g_name[64];
static const char *g_token = "";
static int g_retry = 5;
static const char *g_radio_map = NULL;   /* "phy0=radio0,phy1=radio1" */
static const char *g_nb_cmd = NULL;      /* narrowband hook template   */
static int g_once = 0;
static int g_no_restore = 0;
static int g_verbose = 0;
static int g_sim = 0;
static int g_sim_fail_pct = 0;
static FILE *g_log = NULL;

static volatile sig_atomic_t g_stop = 0;
static int g_backed_up = 0;

/* simulation state */
static int g_sim_channel = 0, g_sim_width = 20, g_sim_freq = 0;
static char g_sim_mode[16] = "";

/* ------------------------------------------------------------------ */
/* logging                                                              */
/* ------------------------------------------------------------------ */

static void log_msg(const char *fmt, ...)
{
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;
	va_list ap;
	FILE *out = g_log ? g_log : stderr;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(out, "[%s] ", ts);
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fputc('\n', out);
	fflush(out);
}

/* ------------------------------------------------------------------ */
/* flat JSON parsing                                                    */
/* ------------------------------------------------------------------ */

struct kv {
	char key[64];
	char val[1024];
};

struct msg {
	int n;
	struct kv kv[MAX_KV];
};

static void skip_ws(const char **p)
{
	while (**p && isspace((unsigned char)**p))
		(*p)++;
}

static int parse_str(const char **p, char *out, size_t sz)
{
	size_t n = 0;

	if (**p != '"')
		return -1;
	(*p)++;
	while (**p && **p != '"') {
		char c = **p;
		if (c == '\\') {
			(*p)++;
			c = **p;
			switch (c) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u': {
				int i;
				for (i = 0; i < 4 && (*p)[1]; i++)
					(*p)++;
				c = '?';
				break;
			}
			case '\0':
				return -1;
			default:
				break; /* \" \\ \/ */
			}
		}
		if (n + 1 < sz)
			out[n++] = c;
		(*p)++;
	}
	if (**p != '"')
		return -1;
	(*p)++;
	out[n] = '\0';
	return 0;
}

static int json_parse_flat(const char *s, struct msg *m)
{
	m->n = 0;
	skip_ws(&s);
	if (*s != '{')
		return -1;
	s++;
	for (;;) {
		struct kv *e;

		skip_ws(&s);
		if (*s == '}')
			return 0;
		if (m->n >= MAX_KV)
			return -1;
		e = &m->kv[m->n];
		if (parse_str(&s, e->key, sizeof(e->key)))
			return -1;
		skip_ws(&s);
		if (*s != ':')
			return -1;
		s++;
		skip_ws(&s);
		if (*s == '"') {
			if (parse_str(&s, e->val, sizeof(e->val)))
				return -1;
		} else if (*s == '{' || *s == '[') {
			return -1; /* nested values are not part of the protocol */
		} else {
			size_t n = 0;
			while (*s && *s != ',' && *s != '}' &&
			       !isspace((unsigned char)*s)) {
				if (n + 1 < sizeof(e->val))
					e->val[n++] = *s;
				s++;
			}
			e->val[n] = '\0';
		}
		m->n++;
		skip_ws(&s);
		if (*s == ',') {
			s++;
			continue;
		}
		if (*s == '}')
			return 0;
		return -1;
	}
}

static const char *msg_get(const struct msg *m, const char *key, const char *def)
{
	int i;

	for (i = 0; i < m->n; i++)
		if (!strcmp(m->kv[i].key, key))
			return m->kv[i].val;
	return def;
}

static int msg_get_int(const struct msg *m, const char *key, int def)
{
	const char *v = msg_get(m, key, NULL);

	if (!v || !*v)
		return def;
	if (!strcmp(v, "true"))
		return 1;
	if (!strcmp(v, "false"))
		return 0;
	return atoi(v);
}

/* ------------------------------------------------------------------ */
/* JSON output builder                                                  */
/* ------------------------------------------------------------------ */

struct jbuf {
	char buf[RES_MAX];
	size_t len;
	int first;
};

static void jb_putc(struct jbuf *b, char c)
{
	if (b->len + 1 < sizeof(b->buf))
		b->buf[b->len++] = c;
	b->buf[b->len] = '\0';
}

static void jb_puts(struct jbuf *b, const char *s)
{
	while (*s)
		jb_putc(b, *s++);
}

static void jb_init(struct jbuf *b)
{
	b->len = 0;
	b->first = 1;
	jb_putc(b, '{');
}

static void jb_key(struct jbuf *b, const char *key)
{
	if (!b->first)
		jb_putc(b, ',');
	b->first = 0;
	jb_putc(b, '"');
	jb_puts(b, key);
	jb_puts(b, "\":");
}

static void jb_str(struct jbuf *b, const char *key, const char *val)
{
	const unsigned char *p = (const unsigned char *)(val ? val : "");

	jb_key(b, key);
	jb_putc(b, '"');
	for (; *p; p++) {
		if (*p == '"' || *p == '\\') {
			jb_putc(b, '\\');
			jb_putc(b, (char)*p);
		} else if (*p == '\n') {
			jb_puts(b, "\\n");
		} else if (*p < 0x20 || *p >= 0x7f) {
			char tmp[8];
			snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
			jb_puts(b, tmp);
		} else {
			jb_putc(b, (char)*p);
		}
	}
	jb_putc(b, '"');
}

static void jb_int(struct jbuf *b, const char *key, long val)
{
	char tmp[32];

	snprintf(tmp, sizeof(tmp), "%ld", val);
	jb_key(b, key);
	jb_puts(b, tmp);
}

static void jb_dbl(struct jbuf *b, const char *key, double val)
{
	char tmp[48];

	snprintf(tmp, sizeof(tmp), "%.3f", val);
	jb_key(b, key);
	jb_puts(b, tmp);
}

static void jb_bool(struct jbuf *b, const char *key, int val)
{
	jb_key(b, key);
	jb_puts(b, val ? "true" : "false");
}

static void jb_end(struct jbuf *b)
{
	jb_puts(b, "}\n");
}

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static double now_s(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Run a shell command, capture stdout+stderr.  Returns exit status. */
static int run_cmd(char *out, size_t outsz, const char *fmt, ...)
{
	char cmd[4096];
	char scratch[512];
	char *buf = out ? out : scratch;
	size_t sz = out ? outsz : sizeof(scratch);
	size_t n = 0;
	va_list ap;
	FILE *fp;
	int st;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd) - 8, fmt, ap);
	va_end(ap);
	strcat(cmd, " 2>&1");

	if (g_verbose)
		log_msg("exec: %s", cmd);

	buf[0] = '\0';
	fp = popen(cmd, "r");
	if (!fp) {
		log_msg("popen failed: %s", strerror(errno));
		return -1;
	}
	for (;;) {
		char tmp[1024];
		size_t r = fread(tmp, 1, sizeof(tmp), fp);
		if (r == 0)
			break;
		if (n + 1 < sz) {
			size_t c = r;
			if (n + c + 1 > sz)
				c = sz - n - 1;
			memcpy(buf + n, tmp, c);
			n += c;
			buf[n] = '\0';
		}
	}
	st = pclose(fp);
	if (st == -1)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void trim(char *s)
{
	size_t n = strlen(s);

	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	while (*s && isspace((unsigned char)*s))
		memmove(s, s + 1, strlen(s));
}

/* Values are interpolated into shell commands: allow only a safe charset. */
static int is_safe(const char *s, int allow_empty)
{
	if (!s)
		return 0;
	if (!*s)
		return allow_empty;
	if (strlen(s) > 63)
		return 0;
	for (; *s; s++) {
		if (!isalnum((unsigned char)*s) && !strchr("._-+@:=,", *s))
			return 0;
	}
	return 1;
}

static int file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* List uci section names of a given type, e.g. ("wireless","wifi-device"). */
static int uci_sections(const char *config, const char *type,
			char names[][64], int max)
{
	static char out[OUT_MAX];
	char suffix[80];
	char *line, *save = NULL;
	int n = 0;

	snprintf(suffix, sizeof(suffix), "=%s", type);
	if (run_cmd(out, sizeof(out), "uci -q -X show %s", config) != 0)
		run_cmd(out, sizeof(out), "uci -q show %s", config);

	for (line = strtok_r(out, "\n", &save); line && n < max;
	     line = strtok_r(NULL, "\n", &save)) {
		char *dot = strchr(line, '.');
		char *eq;
		size_t len;

		if (!dot)
			continue;
		eq = strchr(dot, '=');
		if (!eq || strcmp(eq, suffix))
			continue;
		len = (size_t)(eq - dot - 1);
		if (memchr(dot + 1, '.', len))
			continue;
		if (len >= 64)
			continue;
		memcpy(names[n], dot + 1, len);
		names[n][len] = '\0';
		n++;
	}
	return n;
}

static int list_phys(char names[][64], int max)
{
	static char out[4096];
	char *tok, *save = NULL;
	int n = 0;

	run_cmd(out, sizeof(out), "ls /sys/class/ieee80211/");
	for (tok = strtok_r(out, " \t\n", &save); tok && n < max;
	     tok = strtok_r(NULL, " \t\n", &save)) {
		if (strncmp(tok, "phy", 3))
			continue;
		snprintf(names[n++], 64, "%s", tok);
	}
	return n;
}

/* Map a phy name (phy0) to its uci wifi-device section (radio0). */
static int phy_to_radio(const char *phy, char *radio, size_t sz)
{
	char secs[MAX_SECTIONS][64];
	char out[256];
	int n, i;

	if (g_radio_map) {
		const char *p = g_radio_map;
		size_t plen = strlen(phy);
		while (p && *p) {
			if (!strncmp(p, phy, plen) && p[plen] == '=') {
				const char *v = p + plen + 1;
				size_t l = strcspn(v, ",");
				if (l >= sz)
					l = sz - 1;
				memcpy(radio, v, l);
				radio[l] = '\0';
				return 0;
			}
			p = strchr(p, ',');
			if (p)
				p++;
		}
	}

	n = uci_sections("wireless", "wifi-device", secs, MAX_SECTIONS);
	for (i = 0; i < n; i++) {
		run_cmd(out, sizeof(out), "iwinfo nl80211 phyname '%s'", secs[i]);
		trim(out);
		if (!strcmp(out, phy)) {
			snprintf(radio, sz, "%s", secs[i]);
			return 0;
		}
	}
	/* fallback: phyN <-> radioN */
	if (!strncmp(phy, "phy", 3)) {
		char guess[64];
		snprintf(guess, sizeof(guess), "radio%s", phy + 3);
		for (i = 0; i < n; i++) {
			if (!strcmp(secs[i], guess)) {
				snprintf(radio, sz, "%s", guess);
				return 0;
			}
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* iw parsing                                                           */
/* ------------------------------------------------------------------ */

/* Parse "iw dev fpt0 info" into actual_* fields. Returns 0 if iface exists. */
static int iface_status(struct jbuf *res)
{
	static char out[OUT_MAX];
	const char *p;
	int ch = 0, width = 0;
	double freq = 0, txp = 0;
	char type[32] = "";

	if (g_sim) {
		jb_str(res, "actual_type", g_sim_mode);
		jb_int(res, "actual_channel", g_sim_channel);
		jb_int(res, "actual_freq", g_sim_freq);
		jb_int(res, "actual_width", g_sim_width);
		return g_sim_channel ? 0 : -1;
	}

	if (run_cmd(out, sizeof(out), "iw dev %s info", TEST_IFNAME) != 0)
		return -1;
	p = strstr(out, "\ttype ");
	if (p)
		sscanf(p, "\ttype %31s", type);
	p = strstr(out, "channel ");
	if (p)
		sscanf(p, "channel %d (%lf MHz), width: %d", &ch, &freq, &width);
	p = strstr(out, "txpower ");
	if (p)
		sscanf(p, "txpower %lf", &txp);

	jb_str(res, "actual_type", type);
	jb_int(res, "actual_channel", ch);
	jb_int(res, "actual_freq", (long)freq);
	jb_int(res, "actual_width", width);
	jb_dbl(res, "txpower_dbm", txp);
	return 0;
}

static int parse_signal(const char *out)
{
	const char *p = strstr(out, "signal:");

	if (!p)
		return 0;
	p += 7;
	while (*p == ' ' || *p == '\t')
		p++;
	return atoi(p);
}

static void parse_bitrate(const char *out, const char *label, char *dst, size_t sz)
{
	const char *p = strstr(out, label);
	size_t l;

	dst[0] = '\0';
	if (!p)
		return;
	p += strlen(label);
	while (*p == ' ' || *p == '\t')
		p++;
	l = strcspn(p, "\n");
	if (l >= sz)
		l = sz - 1;
	memcpy(dst, p, l);
	dst[l] = '\0';
}

static int count_substr(const char *hay, const char *needle)
{
	int n = 0;
	const char *p = hay;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/* Collect channel lists / capabilities of one phy. */
static void describe_phy(const char *phy, struct jbuf *res)
{
	static char out[OUT_MAX];
	char chans2[2048] = "", chans5[2048] = "";
	char key[96];
	char *line, *save = NULL;
	int ht40 = 0, vht = 0, he = 0;
	char band[16] = "";
	char radio[64] = "";

	run_cmd(out, sizeof(out), "iw phy %s info", phy);
	for (line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		const char *star, *br;
		double freq;
		int ch, flags = 0;
		char item[32];

		if (strstr(line, "HT20/HT40"))
			ht40 = 1;
		if (strstr(line, "VHT Capabilities"))
			vht = 1;
		if (strstr(line, "HE Iftypes") || strstr(line, "HE MAC Capabilities"))
			he = 1;

		star = strstr(line, "* ");
		br = strstr(line, " MHz [");
		if (!star || !br || br < star)
			continue;
		if (strstr(line, "(disabled)"))
			continue;
		freq = strtod(star + 2, NULL);
		ch = atoi(br + 6);
		if (strstr(line, "radar detection"))
			flags |= 1;
		if (strstr(line, "no IR"))
			flags |= 2;
		snprintf(item, sizeof(item), "%d/%d/%d,", ch, (int)freq, flags);
		if (freq < 2500) {
			if (strlen(chans2) + strlen(item) < sizeof(chans2))
				strcat(chans2, item);
		} else if (freq < 5950) {
			if (strlen(chans5) + strlen(item) < sizeof(chans5))
				strcat(chans5, item);
		}
		/* 6 GHz is intentionally ignored */
	}
	if (*chans2)
		chans2[strlen(chans2) - 1] = '\0';
	if (*chans5)
		chans5[strlen(chans5) - 1] = '\0';
	if (*chans2)
		strcat(band, "2g");
	if (*chans5)
		strcat(band, "5g");

	phy_to_radio(phy, radio, sizeof(radio));

	snprintf(key, sizeof(key), "%s_band", phy);    jb_str(res, key, band);
	snprintf(key, sizeof(key), "%s_chans_2g", phy); jb_str(res, key, chans2);
	snprintf(key, sizeof(key), "%s_chans_5g", phy); jb_str(res, key, chans5);
	snprintf(key, sizeof(key), "%s_ht40", phy);    jb_bool(res, key, ht40);
	snprintf(key, sizeof(key), "%s_vht", phy);     jb_bool(res, key, vht);
	snprintf(key, sizeof(key), "%s_he", phy);      jb_bool(res, key, he);
	snprintf(key, sizeof(key), "%s_uci", phy);     jb_str(res, key, radio);
}

static void sim_describe(struct jbuf *res)
{
	static const char *c2 =
		"1/2412/0,2/2417/0,3/2422/0,4/2427/0,5/2432/0,6/2437/0,7/2442/0,"
		"8/2447/0,9/2452/0,10/2457/0,11/2462/0,12/2467/0,13/2472/0";
	static const char *c5 =
		"36/5180/0,40/5200/0,44/5220/0,48/5240/0,52/5260/1,56/5280/1,"
		"60/5300/1,64/5320/1,100/5500/1,104/5520/1,108/5540/1,112/5560/1,"
		"116/5580/1,120/5600/1,124/5620/1,128/5640/1,132/5660/1,136/5680/1,"
		"140/5700/1,144/5720/1,149/5745/0,153/5765/0,157/5785/0,161/5805/0,165/5825/0";
	static const char *phys[] = { "phy0", "phy1", "phy2" };
	char key[64];
	int i;

	jb_str(res, "radios", "phy0,phy1,phy2");
	for (i = 0; i < 3; i++) {
		int is5 = (i == 0);
		snprintf(key, sizeof(key), "%s_band", phys[i]);    jb_str(res, key, is5 ? "5g" : "2g");
		snprintf(key, sizeof(key), "%s_chans_2g", phys[i]); jb_str(res, key, is5 ? "" : c2);
		snprintf(key, sizeof(key), "%s_chans_5g", phys[i]); jb_str(res, key, is5 ? c5 : "");
		snprintf(key, sizeof(key), "%s_ht40", phys[i]);    jb_bool(res, key, 1);
		snprintf(key, sizeof(key), "%s_vht", phys[i]);     jb_bool(res, key, is5);
		snprintf(key, sizeof(key), "%s_he", phys[i]);      jb_bool(res, key, 1);
		snprintf(key, sizeof(key), "%s_uci", phys[i]);
		snprintf(g_sim_mode, sizeof(g_sim_mode), "radio%d", i);
		jb_str(res, key, g_sim_mode);
	}
	g_sim_mode[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* config backup / restore                                              */
/* ------------------------------------------------------------------ */

static void do_restore(void)
{
	if (g_sim) {
		log_msg("[sim] restore");
		g_backed_up = 0;
		return;
	}
	if (!file_exists(BACKUP_MARKER)) {
		g_backed_up = 0;
		return;
	}
	log_msg("restoring original wireless/network/firewall configuration");
	run_cmd(NULL, 0, "wifi down");
	run_cmd(NULL, 0, "cp " BACKUP_DIR "/wireless /etc/config/wireless");
	run_cmd(NULL, 0, "cp " BACKUP_DIR "/network /etc/config/network");
	if (file_exists(BACKUP_DIR "/firewall"))
		run_cmd(NULL, 0, "cp " BACKUP_DIR "/firewall /etc/config/firewall");
	run_cmd(NULL, 0, "rm -rf " BACKUP_DIR);
	run_cmd(NULL, 0, "/etc/init.d/network reload");
	run_cmd(NULL, 0, "[ -x /etc/init.d/firewall ] && /etc/init.d/firewall reload");
	run_cmd(NULL, 0, "wifi up");
	g_backed_up = 0;
}

static int do_backup(void)
{
	if (file_exists(BACKUP_MARKER)) {
		g_backed_up = 1;
		return 0; /* keep the oldest (original) backup */
	}
	run_cmd(NULL, 0, "mkdir -p " BACKUP_DIR);
	if (run_cmd(NULL, 0, "cp /etc/config/wireless /etc/config/network " BACKUP_DIR "/") != 0)
		return -1;
	run_cmd(NULL, 0, "[ -f /etc/config/firewall ] && cp /etc/config/firewall " BACKUP_DIR "/");
	run_cmd(NULL, 0, "touch " BACKUP_MARKER);
	g_backed_up = 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* ops                                                                  */
/* ------------------------------------------------------------------ */

typedef int (*op_fn)(const struct msg *req, struct jbuf *res, char *err, size_t errsz);

static int op_noop(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	(void)req; (void)err; (void)errsz;
	jb_str(res, "version", AGENT_VERSION);
	return 0;
}

static int op_info(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	char phys[MAX_PHYS][64];
	char list[512] = "";
	char model[128] = "";
	int n, i;

	(void)req; (void)err; (void)errsz;
	jb_str(res, "version", AGENT_VERSION);
	jb_str(res, "hostname", g_name);
	if (g_sim) {
		jb_str(res, "model", "simulated");
		sim_describe(res);
		return 0;
	}
	run_cmd(model, sizeof(model), "cat /tmp/sysinfo/model 2>/dev/null");
	trim(model);
	jb_str(res, "model", model);

	n = list_phys(phys, MAX_PHYS);
	for (i = 0; i < n; i++) {
		if (i)
			strcat(list, ",");
		strcat(list, phys[i]);
	}
	jb_str(res, "radios", list);
	for (i = 0; i < n; i++)
		describe_phy(phys[i], res);
	return 0;
}

static int op_prepare(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	const char *ip = msg_get(req, "ip", "");
	const char *mask = msg_get(req, "netmask", "255.255.255.0");
	int fw = msg_get_int(req, "firewall", 1);
	char secs[MAX_SECTIONS][64];
	int n, i;

	(void)res;
	if (!is_safe(ip, 0) || !is_safe(mask, 0)) {
		snprintf(err, errsz, "invalid ip/netmask");
		return -1;
	}
	if (g_sim) {
		log_msg("[sim] prepare ip=%s", ip);
		g_backed_up = 1;
		return 0;
	}
	if (do_backup()) {
		snprintf(err, errsz, "config backup failed");
		return -1;
	}

	/* dedicated bridge + static IP for test traffic */
	run_cmd(NULL, 0, "uci -q delete network." TEST_NET "_dev");
	run_cmd(NULL, 0, "uci -q delete network." TEST_NET);
	run_cmd(NULL, 0, "uci set network." TEST_NET "_dev=device");
	run_cmd(NULL, 0, "uci set network." TEST_NET "_dev.type=bridge");
	run_cmd(NULL, 0, "uci set network." TEST_NET "_dev.name=br-" TEST_NET);
	run_cmd(NULL, 0, "uci set network." TEST_NET "_dev.bridge_empty=1");
	run_cmd(NULL, 0, "uci set network." TEST_NET "=interface");
	run_cmd(NULL, 0, "uci set network." TEST_NET ".proto=static");
	run_cmd(NULL, 0, "uci set network." TEST_NET ".device=br-" TEST_NET);
	run_cmd(NULL, 0, "uci set network." TEST_NET ".ipaddr='%s'", ip);
	run_cmd(NULL, 0, "uci set network." TEST_NET ".netmask='%s'", mask);
	run_cmd(NULL, 0, "uci commit network");

	if (fw && file_exists("/etc/config/firewall")) {
		run_cmd(NULL, 0, "uci -q delete firewall." TEST_NET "_zone");
		run_cmd(NULL, 0, "uci set firewall." TEST_NET "_zone=zone");
		run_cmd(NULL, 0, "uci set firewall." TEST_NET "_zone.name=" TEST_NET);
		run_cmd(NULL, 0, "uci set firewall." TEST_NET "_zone.input=ACCEPT");
		run_cmd(NULL, 0, "uci set firewall." TEST_NET "_zone.output=ACCEPT");
		run_cmd(NULL, 0, "uci set firewall." TEST_NET "_zone.forward=ACCEPT");
		run_cmd(NULL, 0, "uci add_list firewall." TEST_NET "_zone.network=" TEST_NET);
		run_cmd(NULL, 0, "uci commit firewall");
	}

	/* disable every pre-existing wifi-iface; the test creates its own */
	n = uci_sections("wireless", "wifi-iface", secs, MAX_SECTIONS);
	for (i = 0; i < n; i++)
		run_cmd(NULL, 0, "uci set 'wireless.%s.disabled=1'", secs[i]);
	run_cmd(NULL, 0, "uci commit wireless");

	run_cmd(NULL, 0, "wifi down");
	run_cmd(NULL, 0, "/etc/init.d/network reload");
	if (fw)
		run_cmd(NULL, 0, "[ -x /etc/init.d/firewall ] && /etc/init.d/firewall reload");
	sleep(2);
	log_msg("prepared: test ip %s", ip);
	return 0;
}

static int op_apply(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	const char *phy = msg_get(req, "phy", "");
	const char *mode = msg_get(req, "mode", "");
	const char *htmode = msg_get(req, "htmode", "HT20");
	const char *ssid = msg_get(req, "ssid", "");
	const char *enc = msg_get(req, "encryption", "none");
	const char *key = msg_get(req, "key", "");
	const char *country = msg_get(req, "country", "");
	int channel = msg_get_int(req, "channel", 0);
	int chanbw = msg_get_int(req, "chanbw", 20);
	int wds = msg_get_int(req, "wds", 1);
	char radio[64];
	char secs[MAX_SECTIONS][64];
	int n, i;

	if (!is_safe(phy, 0) || !is_safe(htmode, 0) || !is_safe(ssid, 0) ||
	    !is_safe(enc, 0) || !is_safe(key, 1) || !is_safe(country, 1) ||
	    channel <= 0) {
		snprintf(err, errsz, "invalid parameters");
		return -1;
	}
	if (strcmp(mode, "ap") && strcmp(mode, "sta") && strcmp(mode, "mesh")) {
		snprintf(err, errsz, "invalid mode '%s'", mode);
		return -1;
	}
	if (strcmp(enc, "none") && strlen(key) < 8) {
		snprintf(err, errsz, "key must be >= 8 chars for %s", enc);
		return -1;
	}

	if (g_sim) {
		int bw = 20;
		if (chanbw == 5 || chanbw == 10)
			bw = chanbw;
		else if (strstr(htmode, "80"))
			bw = 80;
		else if (strstr(htmode, "40"))
			bw = 40;
		g_sim_channel = channel;
		g_sim_freq = channel <= 14 ? 2407 + channel * 5 : 5000 + channel * 5;
		g_sim_width = bw;
		snprintf(g_sim_mode, sizeof(g_sim_mode), "%s",
			 !strcmp(mode, "ap") ? "AP" : !strcmp(mode, "sta") ? "managed" : "mesh point");
		log_msg("[sim] apply %s %s ch%d %s bw%d ssid=%s", phy, mode, channel, htmode, chanbw, ssid);
		jb_str(res, "radio", "sim");
		return 0;
	}

	if (phy_to_radio(phy, radio, sizeof(radio))) {
		snprintf(err, errsz, "no uci wifi-device for %s (use -m)", phy);
		return -1;
	}
	jb_str(res, "radio", radio);

	run_cmd(NULL, 0, "wifi down");

	n = uci_sections("wireless", "wifi-device", secs, MAX_SECTIONS);
	for (i = 0; i < n; i++)
		run_cmd(NULL, 0, "uci set 'wireless.%s.disabled=%d'", secs[i],
			strcmp(secs[i], radio) ? 1 : 0);

	run_cmd(NULL, 0, "uci set 'wireless.%s.channel=%d'", radio, channel);
	run_cmd(NULL, 0, "uci set 'wireless.%s.htmode=%s'", radio, htmode);
	if (chanbw == 5 || chanbw == 10)
		run_cmd(NULL, 0, "uci set 'wireless.%s.chanbw=%d'", radio, chanbw);
	else
		run_cmd(NULL, 0, "uci -q delete 'wireless.%s.chanbw'", radio);
	if (*country)
		run_cmd(NULL, 0, "uci set 'wireless.%s.country=%s'", radio, country);

	run_cmd(NULL, 0, "uci -q delete wireless.fpt_iface");
	run_cmd(NULL, 0, "uci set wireless.fpt_iface=wifi-iface");
	run_cmd(NULL, 0, "uci set 'wireless.fpt_iface.device=%s'", radio);
	run_cmd(NULL, 0, "uci set 'wireless.fpt_iface.mode=%s'", mode);
	run_cmd(NULL, 0, "uci set wireless.fpt_iface.ifname=" TEST_IFNAME);
	run_cmd(NULL, 0, "uci set wireless.fpt_iface.network=" TEST_NET);
	if (!strcmp(mode, "mesh")) {
		run_cmd(NULL, 0, "uci set 'wireless.fpt_iface.mesh_id=%s'", ssid);
		run_cmd(NULL, 0, "uci set wireless.fpt_iface.mesh_fwding=1");
		run_cmd(NULL, 0, "uci set wireless.fpt_iface.mesh_rssi_threshold=0");
	} else {
		run_cmd(NULL, 0, "uci set 'wireless.fpt_iface.ssid=%s'", ssid);
		if (wds)
			run_cmd(NULL, 0, "uci set wireless.fpt_iface.wds=1");
	}
	run_cmd(NULL, 0, "uci set 'wireless.fpt_iface.encryption=%s'", enc);
	if (strcmp(enc, "none"))
		run_cmd(NULL, 0, "uci set 'wireless.fpt_iface.key=%s'", key);
	run_cmd(NULL, 0, "uci set wireless.fpt_iface.disabled=0");
	run_cmd(NULL, 0, "uci commit wireless");

	/* optional vendor hook for 5/10 MHz operation: %p phy %r radio %b bw %c channel */
	if (g_nb_cmd && (chanbw == 5 || chanbw == 10)) {
		char cmd[1024];
		size_t o = 0;
		const char *t;
		for (t = g_nb_cmd; *t && o + 64 < sizeof(cmd); t++) {
			if (*t == '%' && t[1]) {
				t++;
				switch (*t) {
				case 'p': o += snprintf(cmd + o, sizeof(cmd) - o, "%s", phy); break;
				case 'r': o += snprintf(cmd + o, sizeof(cmd) - o, "%s", radio); break;
				case 'b': o += snprintf(cmd + o, sizeof(cmd) - o, "%d", chanbw); break;
				case 'c': o += snprintf(cmd + o, sizeof(cmd) - o, "%d", channel); break;
				default: cmd[o++] = *t; break;
				}
			} else {
				cmd[o++] = *t;
			}
		}
		cmd[o] = '\0';
		run_cmd(NULL, 0, "%s", cmd);
	}

	run_cmd(NULL, 0, "wifi up");
	log_msg("applied %s on %s(%s) ch%d %s chanbw=%d", mode, phy, radio, channel, htmode, chanbw);
	return 0;
}

static int op_wait_link(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	static char out[OUT_MAX];
	const char *mode = msg_get(req, "mode", "");
	int timeout = msg_get_int(req, "timeout", 60);
	int min_peers = msg_get_int(req, "min_peers", strcmp(mode, "ap") ? 1 : 0);
	double t0 = now_s();
	int up = 0, peers = 0, signal = 0;
	char txrate[128] = "", rxrate[128] = "";

	if (g_sim) {
		sleep(1);
		if (g_sim_fail_pct && rand() % 100 < g_sim_fail_pct) {
			snprintf(err, errsz, "[sim] link not established within %ds", timeout);
			iface_status(res);
			return -1;
		}
		jb_dbl(res, "link_time", now_s() - t0);
		jb_int(res, "peers", strcmp(mode, "ap") ? 1 : 0);
		jb_int(res, "signal_dbm", -35 - rand() % 20);
		jb_str(res, "tx_bitrate", "simulated");
		iface_status(res);
		return 0;
	}

	while (!g_stop && now_s() - t0 < timeout) {
		if (!strcmp(mode, "sta")) {
			run_cmd(out, sizeof(out), "iw dev %s link", TEST_IFNAME);
			if (strstr(out, "Connected to")) {
				peers = 1;
				signal = parse_signal(out);
				parse_bitrate(out, "tx bitrate:", txrate, sizeof(txrate));
				parse_bitrate(out, "rx bitrate:", rxrate, sizeof(rxrate));
				up = 1;
			}
		} else if (!strcmp(mode, "ap")) {
			run_cmd(out, sizeof(out), "iw dev %s info", TEST_IFNAME);
			if (strstr(out, "type AP") && strstr(out, "channel ")) {
				run_cmd(out, sizeof(out), "iw dev %s station dump", TEST_IFNAME);
				peers = count_substr(out, "Station ");
				signal = parse_signal(out);
				parse_bitrate(out, "tx bitrate:", txrate, sizeof(txrate));
				up = peers >= min_peers;
			}
		} else if (!strcmp(mode, "mesh")) {
			run_cmd(out, sizeof(out), "iw dev %s station dump", TEST_IFNAME);
			peers = count_substr(out, "ESTAB");
			if (peers >= min_peers) {
				signal = parse_signal(out);
				parse_bitrate(out, "tx bitrate:", txrate, sizeof(txrate));
				up = 1;
			}
		} else {
			snprintf(err, errsz, "invalid mode");
			return -1;
		}
		if (up)
			break;
		sleep(1);
	}

	jb_dbl(res, "link_time", now_s() - t0);
	jb_int(res, "peers", peers);
	jb_int(res, "signal_dbm", signal);
	jb_str(res, "tx_bitrate", txrate);
	jb_str(res, "rx_bitrate", rxrate);
	iface_status(res);
	if (!up) {
		snprintf(err, errsz, "%s link not up within %ds (peers=%d)", mode, timeout, peers);
		return -1;
	}
	return 0;
}

static int op_status(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	(void)req;
	if (iface_status(res)) {
		snprintf(err, errsz, "interface %s not present", TEST_IFNAME);
		return -1;
	}
	return 0;
}

static int op_ping(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	static char out[OUT_MAX];
	const char *target = msg_get(req, "target", "");
	int count = msg_get_int(req, "count", 10);
	int size = msg_get_int(req, "size", 56);
	int tx = 0, rx = 0;
	double rmin = 0, ravg = 0, rmax = 0;
	char *line, *save = NULL;

	if (!is_safe(target, 0) || count < 1 || count > 10000 || size < 0 || size > 65000) {
		snprintf(err, errsz, "invalid ping parameters");
		return -1;
	}
	if (g_sim) {
		sleep(1);
		tx = count;
		rx = (g_sim_fail_pct && rand() % 100 < g_sim_fail_pct) ? count / 2 : count;
		rmin = 0.8; ravg = 1.5 + (rand() % 100) / 100.0; rmax = 3.2;
	} else {
		run_cmd(out, sizeof(out), "ping -c %d -W 1 -s %d %s", count, size, target);
		for (line = strtok_r(out, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			const char *p;
			if (strstr(line, "packets transmitted"))
				sscanf(line, "%d packets transmitted, %d", &tx, &rx);
			if ((p = strstr(line, "min/avg/max")) && (p = strstr(p, "= ")))
				sscanf(p + 2, "%lf/%lf/%lf", &rmin, &ravg, &rmax);
		}
	}
	jb_int(res, "ping_tx", tx);
	jb_int(res, "ping_rx", rx);
	jb_dbl(res, "loss_pct", tx ? 100.0 * (tx - rx) / tx : 100.0);
	jb_dbl(res, "rtt_min_ms", rmin);
	jb_dbl(res, "rtt_avg_ms", ravg);
	jb_dbl(res, "rtt_max_ms", rmax);
	if (tx == 0) {
		snprintf(err, errsz, "ping produced no statistics");
		return -1;
	}
	return 0;
}

static int op_teardown(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	(void)req; (void)res; (void)err; (void)errsz;
	if (g_sim) {
		g_sim_channel = 0;
		g_sim_mode[0] = '\0';
		return 0;
	}
	run_cmd(NULL, 0, "wifi down");
	run_cmd(NULL, 0, "uci -q set wireless.fpt_iface.disabled=1");
	run_cmd(NULL, 0, "uci commit wireless");
	return 0;
}

static int op_restore(const struct msg *req, struct jbuf *res, char *err, size_t errsz)
{
	(void)req; (void)res; (void)err; (void)errsz;
	do_restore();
	return 0;
}

static const struct {
	const char *name;
	op_fn fn;
} g_ops[] = {
	{ "noop", op_noop },
	{ "info", op_info },
	{ "prepare", op_prepare },
	{ "apply", op_apply },
	{ "wait_link", op_wait_link },
	{ "status", op_status },
	{ "ping", op_ping },
	{ "teardown", op_teardown },
	{ "restore", op_restore },
};

/* ------------------------------------------------------------------ */
/* networking                                                           */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t w = send(fd, buf, len, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += w;
		len -= (size_t)w;
	}
	return 0;
}

static int connect_server(void)
{
	struct addrinfo hints, *ai, *p;
	char port[16];
	int fd = -1, one = 1, rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port, sizeof(port), "%d", g_port);
	rc = getaddrinfo(g_server, port, &hints, &ai);
	if (rc) {
		log_msg("resolve %s: %s", g_server, gai_strerror(rc));
		return -1;
	}
	for (p = ai; p; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	if (fd < 0)
		return -1;
	/* daemons spawned by "wifi up" must not inherit the control socket */
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

static int send_hello(int fd)
{
	struct jbuf b;

	jb_init(&b);
	jb_str(&b, "type", "hello");
	jb_str(&b, "name", g_name);
	jb_str(&b, "token", g_token);
	jb_str(&b, "version", AGENT_VERSION);
	jb_bool(&b, "sim", g_sim);
	jb_end(&b);
	return send_all(fd, b.buf, b.len);
}

static int handle_line(int fd, const char *line)
{
	static struct msg req;
	static struct jbuf res;
	char err[512] = "";
	const char *type, *op, *id;
	size_t i;
	int rc = -1, found = 0;
	double t0;

	if (json_parse_flat(line, &req)) {
		log_msg("bad message: %.200s", line);
		return 0;
	}
	type = msg_get(&req, "type", "");
	if (!strcmp(type, "bye")) {
		log_msg("controller closed session: %s", msg_get(&req, "reason", ""));
		return -1;
	}
	if (strcmp(type, "cmd"))
		return 0;

	op = msg_get(&req, "op", "");
	id = msg_get(&req, "id", "0");
	jb_init(&res);
	jb_str(&res, "type", "result");
	jb_key(&res, "id");
	jb_puts(&res, isdigit((unsigned char)*id) ? id : "0");
	jb_str(&res, "op", op);

	t0 = now_s();
	if (g_verbose)
		log_msg("op %s id=%s", op, id);
	for (i = 0; i < sizeof(g_ops) / sizeof(g_ops[0]); i++) {
		if (!strcmp(g_ops[i].name, op)) {
			found = 1;
			rc = g_ops[i].fn(&req, &res, err, sizeof(err));
			break;
		}
	}
	if (!found)
		snprintf(err, sizeof(err), "unknown op '%s'", op);
	jb_dbl(&res, "op_time", now_s() - t0);
	jb_bool(&res, "ok", rc == 0);
	if (rc)
		jb_str(&res, "error", err);
	jb_end(&res);
	if (rc)
		log_msg("op %s failed: %s", op, err);
	return send_all(fd, res.buf, res.len);
}

static void session(int fd)
{
	static char rx[RX_BUF_LEN];
	size_t len = 0;

	while (!g_stop) {
		fd_set rfds;
		struct timeval tv = { 1, 0 };
		ssize_t r;
		char *nl;
		int s;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		s = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (s < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (s == 0)
			continue;
		r = recv(fd, rx + len, sizeof(rx) - len - 1, 0);
		if (r <= 0) {
			log_msg("connection closed by controller");
			break;
		}
		len += (size_t)r;
		rx[len] = '\0';
		while ((nl = strchr(rx, '\n')) != NULL) {
			*nl = '\0';
			if (*rx && handle_line(fd, rx) < 0)
				return;
			len -= (size_t)(nl + 1 - rx);
			memmove(rx, nl + 1, len + 1);
		}
		if (len >= sizeof(rx) - 1) {
			log_msg("oversized message dropped");
			len = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"iw-test-agent DUT agent " AGENT_VERSION "\n"
		"usage: %s -s <controller-ip> [options]\n"
		"  -s, --server HOST        controller (laptop) address (required)\n"
		"  -p, --port PORT          controller port (default 5555)\n"
		"  -n, --name NAME          DUT name reported to controller (default hostname)\n"
		"  -t, --token TOKEN        shared token, must match controller --token\n"
		"  -r, --retry SEC          reconnect interval (default 5)\n"
		"  -m, --radio-map MAP      phy->uci map, e.g. phy0=radio0,phy1=radio1\n"
		"  -N, --narrowband-cmd CMD hook run before 'wifi up' for 5/10 MHz tests\n"
		"                           (%%p phy, %%r radio, %%b bandwidth, %%c channel)\n"
		"  -l, --log FILE           log to FILE instead of stderr\n"
		"  -1, --once               exit after the first session ends\n"
		"  -R, --no-restore         do not restore original config on disconnect\n"
		"  -S, --sim                simulation mode (no wireless changes)\n"
		"  -F, --sim-fail PCT       simulated failure rate per operation in %% (with -S)\n"
		"  -v, --verbose            log every executed command\n"
		"  -h, --help               this help\n", prog);
}

int main(int argc, char **argv)
{
	static const struct option lopts[] = {
		{ "server", required_argument, NULL, 's' },
		{ "port", required_argument, NULL, 'p' },
		{ "name", required_argument, NULL, 'n' },
		{ "token", required_argument, NULL, 't' },
		{ "retry", required_argument, NULL, 'r' },
		{ "radio-map", required_argument, NULL, 'm' },
		{ "narrowband-cmd", required_argument, NULL, 'N' },
		{ "log", required_argument, NULL, 'l' },
		{ "once", no_argument, NULL, '1' },
		{ "no-restore", no_argument, NULL, 'R' },
		{ "sim", no_argument, NULL, 'S' },
		{ "sim-fail", required_argument, NULL, 'F' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct sigaction sa;
	int c;

	gethostname(g_name, sizeof(g_name) - 1);

	while ((c = getopt_long(argc, argv, "s:p:n:t:r:m:N:l:1RSF:vh", lopts, NULL)) != -1) {
		switch (c) {
		case 's': g_server = optarg; break;
		case 'p': g_port = atoi(optarg); break;
		case 'n': snprintf(g_name, sizeof(g_name), "%s", optarg); break;
		case 't': g_token = optarg; break;
		case 'r': g_retry = atoi(optarg) > 0 ? atoi(optarg) : 5; break;
		case 'm': g_radio_map = optarg; break;
		case 'N': g_nb_cmd = optarg; break;
		case 'l':
			g_log = fopen(optarg, "a");
			if (!g_log) {
				perror(optarg);
				return 1;
			}
			break;
		case '1': g_once = 1; break;
		case 'R': g_no_restore = 1; break;
		case 'S': g_sim = 1; break;
		case 'F': g_sim_fail_pct = atoi(optarg); break;
		case 'v': g_verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}
	if (!g_server) {
		usage(argv[0]);
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	srand((unsigned)time(NULL) ^ (unsigned)getpid());

	log_msg("iw-test-agent %s name=%s controller=%s:%d%s", AGENT_VERSION,
		g_name, g_server, g_port, g_sim ? " [SIMULATION]" : "");

	/* crash recovery: a previous run left modified config behind */
	if (!g_sim && file_exists(BACKUP_MARKER)) {
		log_msg("found stale backup from previous run");
		do_restore();
	}

	while (!g_stop) {
		int fd = connect_server();
		if (fd < 0) {
			if (g_verbose)
				log_msg("controller %s:%d not reachable, retrying in %ds",
					g_server, g_port, g_retry);
			sleep((unsigned)g_retry);
			continue;
		}
		log_msg("connected to controller %s:%d", g_server, g_port);
		if (send_hello(fd) == 0)
			session(fd);
		close(fd);
		if (g_backed_up && !g_no_restore)
			do_restore();
		if (g_once)
			break;
		if (!g_stop)
			sleep((unsigned)g_retry);
	}
	if (g_backed_up && !g_no_restore)
		do_restore();
	log_msg("agent exiting");
	if (g_log)
		fclose(g_log);
	return 0;
}
