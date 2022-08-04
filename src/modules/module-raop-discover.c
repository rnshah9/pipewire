/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>
#include <pipewire/private.h>
#include <pipewire/i18n.h>

#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include "module-protocol-pulse/format.h"
#include "module-zeroconf-discover/avahi-poll.h"

/** \page page_module_raop_discover PipeWire Module: RAOP Discover
 *
 * Automatically creates RAOP (Airplay) sink devices based on zeroconf
 * information.
 *
 * This module will load module-raop-sink for each discovered sink
 * with the right parameters.
 *
 * ## Module Options
 *
 * This module has no options.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-raop-discover
 *     args = { }
 * }
 * ]
 *\endcode
 *
 * ## See also
 *
 * \ref page_module_raop_sink
 */

#define NAME "raop-discover"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	" "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Discover remote streams" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define SERVICE_TYPE_SINK "_raop._tcp"

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_properties *properties;

	AvahiPoll *avahi_poll;
	AvahiClient *client;
	AvahiServiceBrowser *sink_browser;

	struct spa_list tunnel_list;
};

struct tunnel_info {
	AvahiIfIndex interface;
	AvahiProtocol protocol;
	const char *name;
	const char *type;
	const char *domain;
};

#define TUNNEL_INFO(...) (struct tunnel_info){ __VA_ARGS__ }

struct tunnel {
	struct spa_list link;
	struct tunnel_info info;
	struct pw_impl_module *module;
	struct spa_hook module_listener;
};

static int start_client(struct impl *impl);

static struct tunnel *make_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;

	t->info.interface = info->interface;
	t->info.protocol = info->protocol;
	t->info.name = strdup(info->name);
	t->info.type = strdup(info->type);
	t->info.domain = strdup(info->domain);
	spa_list_append(&impl->tunnel_list, &t->link);

	return t;
}

static struct tunnel *find_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnel_list, link) {
		if (t->info.interface == info->interface &&
		    t->info.protocol == info->protocol &&
		    spa_streq(t->info.name, info->name) &&
		    spa_streq(t->info.type, info->type) &&
		    spa_streq(t->info.domain, info->domain))
			return t;
	}
	return NULL;
}

static void free_tunnel(struct tunnel *t)
{
	pw_impl_module_destroy(t->module);
}

static void impl_free(struct impl *impl)
{
	struct tunnel *t;

	spa_list_consume(t, &impl->tunnel_list, link)
		free_tunnel(t);

	if (impl->sink_browser)
		avahi_service_browser_free(impl->sink_browser);
	if (impl->client)
		avahi_client_free(impl->client);
	if (impl->avahi_poll)
		pw_avahi_poll_free(impl->avahi_poll);
	pw_properties_free(impl->properties);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static bool str_in_list(const char *haystack, const char *delimiters, const char *needle)
{
	const char *s, *state = NULL;
	size_t len;
	while ((s = pw_split_walk(haystack, delimiters, &len, &state))) {
		if (spa_strneq(needle, s, len))
	            return true;
	}
	return false;
}

static void pw_properties_from_avahi_string(const char *key, const char *value,
		struct pw_properties *props)
{
	if (spa_streq(key, "device")) {
		pw_properties_set(props, "raop.device", value);
	}
	else if (spa_streq(key, "tp")) {
		/* transport protocol, "UDP", "TCP", "UDP,TCP" */
		if (str_in_list(value, ",", "UDP"))
			value = "udp";
		else if (str_in_list(value, ",", "TCP"))
			value = "tcp";
		pw_properties_set(props, "raop.transport", value);
	} else if (spa_streq(key, "et")) {
		/* Supported encryption types:
		 *  0 = none,
		 *  1 = RSA,
		 *  2 = FairPlay,
		 *  3 = MFiSAP,
		 *  4 = FairPlay SAPv2.5. */
		if (str_in_list(value, ",", "1"))
			value = "RSA";
		else if (str_in_list(value, ",", "4"))
			value = "auth_setup";
		else
			value = "none";
		pw_properties_set(props, "raop.encryption.type", value);
	} else if (spa_streq(key, "cn")) {
		/* Suported audio codecs:
		 *  0 = PCM,
		 *  1 = ALAC,
		 *  2 = AAC,
		 *  3 = AAC ELD. */
		if (str_in_list(value, ",", "0"))
			value = "PCM";
		else if (str_in_list(value, ",", "1"))
			value = "ALAC";
		else if (str_in_list(value, ",", "2"))
			value = "AAC";
		else if (str_in_list(value, ",", "2"))
			value = "AAC-ELD";
		else
			value = "unknown";
		pw_properties_set(props, "raop.audio.codec", value);
	} else if (spa_streq(key, "ch")) {
		/* Number of channels */
		pw_properties_set(props, PW_KEY_AUDIO_CHANNELS, value);
	} else if (spa_streq(key, "ss")) {
		/* Sample size */
		if (spa_streq(value, "16"))
			value = "S16";
		else if (spa_streq(value, "24"))
			value = "S24";
		else if (spa_streq(value, "32"))
			value = "S32";
		else
			value = "UNKNOWN";
		pw_properties_set(props, PW_KEY_AUDIO_FORMAT, value);
	} else if (spa_streq(key, "sr")) {
		/* Sample rate */
		pw_properties_set(props, PW_KEY_AUDIO_RATE, value);
	} else if (spa_streq(key, "am")) {
		/* Device model */
		pw_properties_set(props, "device.model", value);
        }
}

static void submodule_destroy(void *data)
{
	struct tunnel *t = data;

	spa_list_remove(&t->link);
	spa_hook_remove(&t->module_listener);

	free((char *) t->info.name);
	free((char *) t->info.type);
	free((char *) t->info.domain);

	free(t);
}

static const struct pw_impl_module_events submodule_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = submodule_destroy,
};

static void resolver_cb(AvahiServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiResolverEvent event, const char *name, const char *type, const char *domain,
	const char *host_name, const AvahiAddress *a, uint16_t port, AvahiStringList *txt,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel *t;
	struct tunnel_info tinfo;
	const char *str;
	AvahiStringList *l;
	FILE *f;
	char *args;
	size_t size;
	struct pw_impl_module *mod;
	struct pw_properties *props = NULL;
	char at[AVAHI_ADDRESS_STR_MAX];

	if (event != AVAHI_RESOLVER_FOUND) {
		pw_log_error("Resolving of '%s' failed: %s", name,
				avahi_strerror(avahi_client_errno(impl->client)));
		goto done;
	}
	tinfo = TUNNEL_INFO(.interface = interface,
			.protocol = protocol,
			.name = name,
			.type = type,
			.domain = domain);

	props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		pw_log_error("Can't allocate properties: %m");
		goto done;
	}

	avahi_address_snprint(at, sizeof(at), a);

	pw_properties_setf(props, "raop.hostname", "%s", at);
	pw_properties_setf(props, "raop.port", "%u", port);

	if ((str = strstr(name, "@"))) {
		str++;
		if (strlen(str) > 0)
			pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, str);
		else
			pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
					"RAOP on %s", host_name);
	}

	for (l = txt; l; l = l->next) {
		char *key, *value;

		if (avahi_string_list_get_pair(l, &key, &value, NULL) != 0)
			break;

		pw_properties_from_avahi_string(key, value, props);
		avahi_free(key);
		avahi_free(value);
	}


	if ((f = open_memstream(&args, &size)) == NULL) {
		pw_log_error("Can't open memstream: %m");
		goto done;
	}

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &props->dict, 0);
	fprintf(f, " stream.props = {");
	fprintf(f, " }");
	fprintf(f, "}");
        fclose(f);

	pw_properties_free(props);

	pw_log_info("loading module args:'%s'", args);
	mod = pw_context_load_module(impl->context,
			"libpipewire-module-raop-sink",
			args, NULL);
	free(args);

	if (mod == NULL) {
		pw_log_error("Can't load module: %m");
                goto done;
	}

	t = make_tunnel(impl, &tinfo);
	if (t == NULL) {
		pw_log_error("Can't make tunnel: %m");
		pw_impl_module_destroy(mod);
		goto done;
	}

	pw_impl_module_add_listener(mod, &t->module_listener, &submodule_events, t);

	t->module = mod;

done:
	avahi_service_resolver_free(r);
}


static void browser_cb(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel_info info;
	struct tunnel *t;

	if (flags & AVAHI_LOOKUP_RESULT_LOCAL)
		return;

	info = TUNNEL_INFO(.interface = interface,
			.protocol = protocol,
			.name = name,
			.type = type,
			.domain = domain);

	t = find_tunnel(impl, &info);

	switch (event) {
	case AVAHI_BROWSER_NEW:
		if (t != NULL)
			return;
		if (!(avahi_service_resolver_new(impl->client,
						interface, protocol,
						name, type, domain,
						AVAHI_PROTO_UNSPEC, 0,
						resolver_cb, impl)))
			pw_log_error("can't make service resolver: %s",
					avahi_strerror(avahi_client_errno(impl->client)));
		break;
	case AVAHI_BROWSER_REMOVE:
		if (t == NULL)
			return;
		free_tunnel(t);
		break;
	default:
		break;
	}
}


static struct AvahiServiceBrowser *make_browser(struct impl *impl, const char *service_type)
{
	struct AvahiServiceBrowser *s;

	s = avahi_service_browser_new(impl->client,
                              AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                              service_type, NULL, 0,
                              browser_cb, impl);
	if (s == NULL) {
		pw_log_error("can't make browser for %s: %s", service_type,
				avahi_strerror(avahi_client_errno(impl->client)));
	}
	return s;
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata)
{
	struct impl *impl = userdata;

	impl->client = c;

	switch (state) {
	case AVAHI_CLIENT_S_REGISTERING:
	case AVAHI_CLIENT_S_RUNNING:
	case AVAHI_CLIENT_S_COLLISION:
		if (impl->sink_browser == NULL)
			impl->sink_browser = make_browser(impl, SERVICE_TYPE_SINK);
		if (impl->sink_browser == NULL)
			goto error;
		break;
	case AVAHI_CLIENT_FAILURE:
		if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED)
			start_client(impl);

		SPA_FALLTHROUGH;
	case AVAHI_CLIENT_CONNECTING:
		if (impl->sink_browser) {
			avahi_service_browser_free(impl->sink_browser);
			impl->sink_browser = NULL;
		}
		break;
	default:
		break;
	}
	return;
error:
	pw_impl_module_schedule_destroy(impl->module);
}

static int start_client(struct impl *impl)
{
	int res;
	if ((impl->client = avahi_client_new(impl->avahi_poll,
					AVAHI_CLIENT_NO_FAIL,
					client_callback, impl,
					&res)) == NULL) {
		pw_log_error("can't create client: %s", avahi_strerror(res));
		pw_impl_module_schedule_destroy(impl->module);
		return -EIO;
	}
	return 0;
}

static int start_avahi(struct impl *impl)
{
	struct pw_loop *loop;

	loop = pw_context_get_main_loop(impl->context);
	impl->avahi_poll = pw_avahi_poll_new(loop);

	return start_client(impl);
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL)
		goto error_errno;

	spa_list_init(&impl->tunnel_list);

	impl->module = module;
	impl->context = context;
	impl->properties = props;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	start_avahi(impl);

	return 0;

error_errno:
	res = -errno;
	if (impl)
		impl_free(impl);
	return res;
}
