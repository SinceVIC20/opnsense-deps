/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mono Technologies Inc.
 *
 * Fan control daemon for the Mono Gateway.
 *
 * Each fan follows a curve.  A curve is a list of temperature:speed
 * points and the hw.temperature sensors it reads; it uses the hottest of
 * them.  Between points the speed rises evenly, below the first and above
 * the last the nearest point's speed is used.  Curves and fans are read
 * from a config file and read again on SIGHUP:
 *
 *	curve standard sensors core-cluster points 40:20,80:100
 *	fan 0 standard
 *	fan 1 off
 *
 * Without a config file every fan follows the standard curve above.
 */

#include <sys/types.h>
#include <sys/sysctl.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define	CONF_FILE	"/usr/local/etc/fand.conf"
#define	DEF_INTERVAL	2	/* seconds */

#define	MAX_FANS	5	/* EMC2305 has the most channels */
#define	MAX_CURVES	16
#define	MAX_POINTS	32
#define	MAX_SENSORS	16
#define	NAME_LEN	65

/* Above this the fans always run at 100%, whatever the curve says */
#define	TEMP_CRIT_MC	90000	/* 90°C */

/* Highest temperature a curve point may use, as on the settings page */
#define	POINT_TEMP_MAX	110

/* Readings outside this range are treated as a sensor that cannot be read */
#define	TEMP_MIN_MC	(-40000)
#define	TEMP_MAX_MC	150000

#define	DK_ZERO_C	2731	/* 0°C in decikelvin */

#define	PWM_FULL	255

#define	FAN_UNSET	(-2)	/* not in the config: left alone */
#define	FAN_OFF		(-1)	/* not connected: held at 0 */

#define	SYSCTL_TEMP	"hw.temperature."
#define	SYSCTL_FAN_PWM	"dev.emc2302.0.fan%d.pwm"

struct point {
	int	temp_mc;
	int	pwm;
};

struct curve {
	char		name[NAME_LEN];
	char		sensors[MAX_SENSORS][NAME_LEN];
	int		nsensors;
	struct point	points[MAX_POINTS];
	int		npoints;
};

struct config {
	struct curve	curves[MAX_CURVES];
	int		ncurves;
	int		fan_curve[MAX_FANS];
};

static const char *default_conf[] = {
	"curve standard sensors core-cluster points 40:20,80:100",
	"fan 0 standard",
	"fan 1 standard",
	NULL
};

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload;
static int dflag;

static void	logmsg(int, const char *, ...) __printflike(2, 3);

static void
signal_handler(int sig)
{

	if (sig == SIGHUP)
		reload = 1;
	else
		running = 0;
}

static void
logmsg(int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (dflag)
		vsyslog(pri, fmt, ap);
	else {
		fprintf(stderr, "fand: ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
	va_end(ap);
}

/* ================================================================
 * Config parsing
 * ================================================================ */

static int
valid_name(const char *s)
{

	if (*s == '\0' || strlen(s) >= NAME_LEN)
		return (0);
	for (; *s != '\0'; s++)
		if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
			return (0);
	return (1);
}

static int
find_curve(const struct config *cf, const char *name)
{
	int i;

	for (i = 0; i < cf->ncurves; i++)
		if (strcmp(cf->curves[i].name, name) == 0)
			return (i);
	return (-1);
}

static int
cmp_point(const void *a, const void *b)
{

	return (((const struct point *)a)->temp_mc -
	    ((const struct point *)b)->temp_mc);
}

static int
parse_sensors(struct curve *c, char *list, char *err, size_t errlen)
{
	char *tok, *last;

	for (tok = strtok_r(list, ",", &last); tok != NULL;
	    tok = strtok_r(NULL, ",", &last)) {
		if (c->nsensors == MAX_SENSORS) {
			snprintf(err, errlen, "more than %d sensors",
			    MAX_SENSORS);
			return (-1);
		}
		if (!valid_name(tok)) {
			snprintf(err, errlen, "bad sensor name '%s'", tok);
			return (-1);
		}
		strlcpy(c->sensors[c->nsensors++], tok, NAME_LEN);
	}
	if (c->nsensors == 0) {
		snprintf(err, errlen, "no sensors");
		return (-1);
	}
	return (0);
}

static int
parse_points(struct curve *c, char *list, char *err, size_t errlen)
{
	char *tok, *last, *end;
	long t, p;
	int i;

	for (tok = strtok_r(list, ",", &last); tok != NULL;
	    tok = strtok_r(NULL, ",", &last)) {
		if (c->npoints == MAX_POINTS) {
			snprintf(err, errlen, "more than %d points",
			    MAX_POINTS);
			return (-1);
		}
		t = strtol(tok, &end, 10);
		if (end == tok || *end != ':' || t < 0 || t > POINT_TEMP_MAX) {
			snprintf(err, errlen, "point '%s' is not temp:speed "
			    "with a temperature from 0 to %d", tok,
			    POINT_TEMP_MAX);
			return (-1);
		}
		tok = end + 1;
		p = strtol(tok, &end, 10);
		if (end == tok || *end != '\0' || p < 0 || p > 100) {
			snprintf(err, errlen, "speed '%s' is not from 0 to "
			    "100", tok);
			return (-1);
		}
		c->points[c->npoints].temp_mc = (int)t * 1000;
		c->points[c->npoints].pwm = (int)p * PWM_FULL / 100;
		c->npoints++;
	}
	if (c->npoints == 0) {
		snprintf(err, errlen, "no points");
		return (-1);
	}
	qsort(c->points, c->npoints, sizeof(c->points[0]), cmp_point);
	for (i = 1; i < c->npoints; i++)
		if (c->points[i].temp_mc == c->points[i - 1].temp_mc) {
			snprintf(err, errlen, "%d°C is given twice",
			    c->points[i].temp_mc / 1000);
			return (-1);
		}
	return (0);
}

/* Parses one line into cf.  Returns -1 with a reason in err. */
static int
parse_line(struct config *cf, const char *line, char *err, size_t errlen)
{
	char buf[2048], *word[6], *last, *p;
	struct curve *c;
	long fan;
	int n, i;

	if (strlcpy(buf, line, sizeof(buf)) >= sizeof(buf)) {
		snprintf(err, errlen, "line too long");
		return (-1);
	}
	if ((p = strchr(buf, '#')) != NULL)
		*p = '\0';

	n = 0;
	for (p = strtok_r(buf, " \t\r\n", &last); p != NULL;
	    p = strtok_r(NULL, " \t\r\n", &last)) {
		if (n == 6) {
			snprintf(err, errlen, "too many words");
			return (-1);
		}
		word[n++] = p;
	}
	if (n == 0)
		return (0);

	if (strcmp(word[0], "curve") == 0) {
		if (n != 6 || strcmp(word[2], "sensors") != 0 ||
		    strcmp(word[4], "points") != 0) {
			snprintf(err, errlen, "expected: curve NAME sensors "
			    "S1,S2 points T:P,T:P");
			return (-1);
		}
		if (!valid_name(word[1])) {
			snprintf(err, errlen, "bad curve name '%s'", word[1]);
			return (-1);
		}
		if (find_curve(cf, word[1]) >= 0) {
			snprintf(err, errlen, "curve '%s' is given twice",
			    word[1]);
			return (-1);
		}
		if (cf->ncurves == MAX_CURVES) {
			snprintf(err, errlen, "more than %d curves",
			    MAX_CURVES);
			return (-1);
		}
		c = &cf->curves[cf->ncurves];
		memset(c, 0, sizeof(*c));
		strlcpy(c->name, word[1], NAME_LEN);
		if (parse_sensors(c, word[3], err, errlen) != 0 ||
		    parse_points(c, word[5], err, errlen) != 0)
			return (-1);
		cf->ncurves++;
		return (0);
	}

	if (strcmp(word[0], "fan") == 0) {
		if (n != 3) {
			snprintf(err, errlen, "expected: fan N CURVE|off");
			return (-1);
		}
		fan = strtol(word[1], &p, 10);
		if (*word[1] == '\0' || *p != '\0' || fan < 0 ||
		    fan >= MAX_FANS) {
			snprintf(err, errlen, "fan must be from 0 to %d",
			    MAX_FANS - 1);
			return (-1);
		}
		if (strcmp(word[2], "off") == 0) {
			cf->fan_curve[fan] = FAN_OFF;
			return (0);
		}
		if ((i = find_curve(cf, word[2])) < 0) {
			snprintf(err, errlen, "unknown curve '%s' (define it "
			    "before the fan)", word[2]);
			return (-1);
		}
		cf->fan_curve[fan] = i;
		return (0);
	}

	snprintf(err, errlen, "unknown keyword '%s'", word[0]);
	return (-1);
}

static void
config_init(struct config *cf)
{
	int i;

	memset(cf, 0, sizeof(*cf));
	for (i = 0; i < MAX_FANS; i++)
		cf->fan_curve[i] = FAN_UNSET;
}

/* Every fan on the standard curve, as fand has always done */
static void
config_default(struct config *cf)
{
	char err[160];
	int i;

	config_init(cf);
	for (i = 0; default_conf[i] != NULL; i++)
		(void)parse_line(cf, default_conf[i], err, sizeof(err));
}

static int
has_fans(const struct config *cf)
{
	int i;

	for (i = 0; i < MAX_FANS; i++)
		if (cf->fan_curve[i] != FAN_UNSET)
			return (1);
	return (0);
}

/*
 * Reads the config file, or the built-in default when there is none.
 * Returns -1 and logs the reason if the file has an error.
 */
static int
config_load(struct config *cf, const char *path)
{
	char line[2048], err[160];
	FILE *fp;
	int lineno, rv;

	config_init(cf);
	if ((fp = fopen(path, "r")) == NULL) {
		if (errno != ENOENT) {
			logmsg(LOG_ERR, "%s: %s", path, strerror(errno));
			return (-1);
		}
		config_default(cf);
		return (0);
	}

	rv = 0;
	lineno = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		lineno++;
		if (strchr(line, '\n') == NULL && !feof(fp))
			snprintf(err, sizeof(err), "line too long");
		else if (parse_line(cf, line, err, sizeof(err)) == 0)
			continue;
		logmsg(LOG_ERR, "%s line %d: %s", path, lineno, err);
		rv = -1;
		break;
	}
	if (rv == 0 && ferror(fp)) {
		logmsg(LOG_ERR, "%s: %s", path, strerror(errno));
		rv = -1;
	}
	fclose(fp);
	if (rv == 0 && !has_fans(cf)) {
		logmsg(LOG_ERR, "%s: no fan is set", path);
		rv = -1;
	}
	return (rv);
}

/* ================================================================
 * Sensors and fans
 * ================================================================ */

static int
read_sensor_mc(const char *name, int *temp_mc)
{
	char oid[96];
	int val;
	size_t len;

	snprintf(oid, sizeof(oid), SYSCTL_TEMP "%s", name);
	len = sizeof(val);
	if (sysctlbyname(oid, &val, &len, NULL, 0) != 0 || len != sizeof(val))
		return (-1);

	/* A reading no router can have means the sensor is broken */
	if (val < DK_ZERO_C + TEMP_MIN_MC / 100 ||
	    val > DK_ZERO_C + TEMP_MAX_MC / 100)
		return (-1);

	/* Convert decikelvin to millidegrees C */
	*temp_mc = (val - DK_ZERO_C) * 100;
	return (0);
}

/*
 * Speed for a curve from the hottest of its sensors.  Sensors that
 * cannot be read are skipped; when none can be read the result is 100%.
 */
static int
curve_pwm(const struct curve *c, int *temp_mc)
{
	const struct point *p;
	int i, t, found;

	found = 0;
	for (i = 0; i < c->nsensors; i++) {
		if (read_sensor_mc(c->sensors[i], &t) != 0)
			continue;
		if (!found || t > *temp_mc)
			*temp_mc = t;
		found = 1;
	}
	if (!found)
		return (-1);

	p = c->points;
	if (*temp_mc >= TEMP_CRIT_MC)
		return (PWM_FULL);
	if (*temp_mc <= p[0].temp_mc)
		return (p[0].pwm);
	for (i = 1; i < c->npoints; i++)
		if (*temp_mc < p[i].temp_mc)
			return (p[i - 1].pwm + (*temp_mc - p[i - 1].temp_mc) *
			    (p[i].pwm - p[i - 1].pwm) /
			    (p[i].temp_mc - p[i - 1].temp_mc));
	return (p[c->npoints - 1].pwm);
}

static int
set_pwm(int fan, int pwm)
{
	char oid[64];

	snprintf(oid, sizeof(oid), SYSCTL_FAN_PWM, fan);
	return (sysctlbyname(oid, NULL, NULL, &pwm, sizeof(pwm)));
}

static int
get_pwm(int fan, int *pwm)
{
	char oid[64];
	size_t len;

	snprintf(oid, sizeof(oid), SYSCTL_FAN_PWM, fan);
	len = sizeof(*pwm);
	return (sysctlbyname(oid, pwm, &len, NULL, 0));
}

/* Full speed, with one retry since this is the safe state */
static void
fan_full(int fan)
{

	if (set_pwm(fan, PWM_FULL) != 0 && set_pwm(fan, PWM_FULL) != 0)
		logmsg(LOG_ERR, "fan%d: cannot set it to 100%%: %s", fan,
		    strerror(errno));
}

static void
all_fans_full(const struct config *cf)
{
	int i;

	for (i = 0; i < MAX_FANS; i++)
		if (cf->fan_curve[i] != FAN_UNSET)
			fan_full(i);
}

static void
log_config(const struct config *cf)
{
	int i;

	for (i = 0; i < MAX_FANS; i++) {
		if (cf->fan_curve[i] == FAN_OFF)
			logmsg(LOG_NOTICE, "fan%d: not connected", i);
		else if (cf->fan_curve[i] >= 0)
			logmsg(LOG_NOTICE, "fan%d: curve %s", i,
			    cf->curves[cf->fan_curve[i]].name);
	}
}

static void
usage(void)
{

	fprintf(stderr, "usage: fand [-d] [-c file] [-i secs]\n"
	    "  -d  daemonize\n"
	    "  -c  config file (" CONF_FILE ")\n"
	    "  -i  polling interval in seconds (%d)\n"
	    "Send SIGHUP to read the config file again.\n", DEF_INTERVAL);
	exit(1);
}

int
main(int argc, char *argv[])
{
	static struct config cf, newcf;
	const char *path;
	char *end;
	int ch, i, fan, interval, temp_mc, pwm[MAX_CURVES], temp[MAX_CURVES];
	int want, cur, last_pwm[MAX_FANS], set_failed[MAX_FANS];
	int sensor_failed[MAX_FANS];
	long v;

	/* Before anything else, so an early SIGHUP cannot end fand */
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGHUP, signal_handler);

	path = CONF_FILE;
	interval = DEF_INTERVAL;

	while ((ch = getopt(argc, argv, "dc:i:")) != -1) {
		switch (ch) {
		case 'd':
			dflag = 1;
			break;
		case 'c':
			path = optarg;
			break;
		case 'i':
			v = strtol(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' || v < 1 || v > 60)
				errx(1, "-i must be from 1 to 60 seconds");
			interval = (int)v;
			break;
		default:
			usage();
		}
	}
	if (argc != optind)
		usage();

	if (config_load(&cf, path) != 0) {
		/* Never leave the fans without control: use the default */
		warnx("cannot use %s, every fan follows the standard curve",
		    path);
		config_default(&cf);
	}
	for (fan = 0; fan < MAX_FANS; fan++)
		if (cf.fan_curve[fan] != FAN_UNSET && get_pwm(fan, &cur) != 0)
			errx(1, "fan%d is not there (is emc2302 loaded?)", fan);

	if (dflag) {
		if (daemon(0, 0) != 0)
			err(1, "daemon");
		openlog("fand", LOG_PID, LOG_DAEMON);
	}

	log_config(&cf);
	for (i = 0; i < MAX_FANS; i++) {
		last_pwm[i] = -1;
		set_failed[i] = 0;
		sensor_failed[i] = 0;
	}

	while (running) {
		if (reload) {
			reload = 0;
			if (config_load(&newcf, path) == 0) {
				/* A fan left out of the new config: full speed */
				for (fan = 0; fan < MAX_FANS; fan++)
					if (cf.fan_curve[fan] != FAN_UNSET &&
					    newcf.fan_curve[fan] == FAN_UNSET)
						fan_full(fan);
				cf = newcf;
				for (i = 0; i < MAX_FANS; i++) {
					last_pwm[i] = -1;
					sensor_failed[i] = 0;
				}
				logmsg(LOG_NOTICE, "read %s again", path);
				log_config(&cf);
			} else
				logmsg(LOG_ERR, "keeping the previous config");
		}

		for (i = 0; i < cf.ncurves; i++)
			pwm[i] = curve_pwm(&cf.curves[i], &temp[i]);

		for (fan = 0; fan < MAX_FANS; fan++) {
			i = cf.fan_curve[fan];
			if (i == FAN_UNSET)
				continue;
			if (i == FAN_OFF) {
				want = 0;
			} else if (pwm[i] < 0) {
				/* Sensor failure: full speed for safety */
				if (!sensor_failed[fan])
					logmsg(LOG_WARNING, "fan%d: cannot read "
					    "any sensor of curve %s, fan to 100%%",
					    fan, cf.curves[i].name);
				sensor_failed[fan] = 1;
				want = PWM_FULL;
			} else {
				if (sensor_failed[fan])
					logmsg(LOG_NOTICE, "fan%d: sensors of "
					    "curve %s can be read again", fan,
					    cf.curves[i].name);
				sensor_failed[fan] = 0;
				want = pwm[i];
			}

			/* Skip small changes, but always reach 0 and 100% */
			if (last_pwm[fan] >= 0 && (want == last_pwm[fan] ||
			    (abs(want - last_pwm[fan]) < 5 && want != 0 &&
			    want != PWM_FULL)))
				continue;

			/* Not marked as done when it fails, so it is tried again */
			if (set_pwm(fan, want) != 0) {
				if (!set_failed[fan])
					logmsg(LOG_ERR, "fan%d: cannot set PWM %d: "
					    "%s", fan, want, strerror(errno));
				set_failed[fan] = 1;
				continue;
			}
			if (set_failed[fan])
				logmsg(LOG_NOTICE, "fan%d: PWM can be set again",
				    fan);
			set_failed[fan] = 0;
			last_pwm[fan] = want;

			if (i >= 0 && pwm[i] >= 0) {
				temp_mc = temp[i];
				logmsg(LOG_DEBUG, "fan%d: %d.%d°C on curve %s, "
				    "PWM %d (%d%%)", fan, temp_mc / 1000,
				    abs(temp_mc % 1000) / 100,
				    cf.curves[i].name, want,
				    want * 100 / PWM_FULL);
			}
		}

		sleep(interval);
	}

	/* Safety: full speed on exit */
	logmsg(LOG_NOTICE, "shutting down, fans to 100%%");
	all_fans_full(&cf);

	return (0);
}
