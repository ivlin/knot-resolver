/*  Copyright (C) 2016 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <ccan/json/json.h>
#include <libknot/db/db_lmdb.h>
#include <stdlib.h>
#include <string.h>

#include "daemon/engine.h"
#include "lib/cookies/alg_clnt.h"
#include "lib/cookies/alg_srvr.h"
#include "lib/cookies/control.h"
#include "lib/layer.h"

#define DEBUG_MSG(qry, fmt...) QRDEBUG(qry, "cookiectl",  fmt)

#define NAME_ENABLED "enabled"
#define NAME_CLIENT_SECRET "client_secret"
#define NAME_CLIENT_COOKIE_ALG "client_cookie_alg"
#define NAME_AVAILABLE_CLIENT_COOKIE_ALGS "available_client_cookie_algs"
#define NAME_CACHE_TTL "cache_ttl"

static bool aply_enabled(struct kr_cookie_ctx *cntrl, const JsonNode *node)
{
	if (node->tag == JSON_BOOL) {
		cntrl->enabled = node->bool_;
		return true;
	}

	return false;
}

static struct kr_cookie_secret *new_sq_str(const JsonNode *node)
{
	assert(node && node->tag == JSON_STRING);

	size_t len = strlen(node->string_);

	struct kr_cookie_secret *sq = malloc(sizeof(*sq) + len);
	if (!sq) {
		return NULL;
	}
	sq->size = len;
	memcpy(sq->data, node->string_, len);

	return sq;
}

#define holds_char(x) ((x) >= 0 && (x) <= 255)

static struct kr_cookie_secret *new_sq_array(const JsonNode *node)
{
	assert(node && node->tag == JSON_ARRAY);

	const JsonNode *element = NULL;
	size_t cnt = 0;
	json_foreach(element, node) {
		if (element->tag != JSON_NUMBER || !holds_char(element->number_)) {
			return NULL;
		}
		++cnt;
	}
	if (cnt == 0) {
		return NULL;
	}

	struct kr_cookie_secret *sq = malloc(sizeof(*sq) + cnt);
	if (!sq) {
		return NULL;
	}

	sq->size = cnt;
	cnt = 0;
	json_foreach(element, node) {
		sq->data[cnt++] = (uint8_t) element->number_;
	}

	return sq;
}

static bool apply_client_secret(struct kr_cookie_ctx *cntrl,
                                const JsonNode *node)
{
	struct kr_cookie_secret *sq = NULL;

	switch (node->tag) {
	case JSON_STRING:
		sq = new_sq_str(node);
		break;
	case JSON_ARRAY:
		sq = new_sq_array(node);
		break;
	default:
		break;
	}

	if (!sq) {
		return false;
	}

	if (sq->size == cntrl->current_cs->size &&
	    memcmp(sq->data, cntrl->current_cs->data, sq->size) == 0) {
		/* Ignore same values. */
		free(sq);
		return true;
	}

	struct kr_cookie_secret *tmp = cntrl->recent_cs;
	cntrl->recent_cs = cntrl->current_cs;
	cntrl->current_cs = sq;

	if (tmp && tmp != &dflt_cs) {
		free(tmp);
	}

	return true;
}

static bool apply_client_hash_func(struct kr_cookie_ctx *cntrl,
                                   const JsonNode *node)
{
	if (node->tag == JSON_STRING) {
		const struct kr_clnt_cookie_alg_descr *cc_alg = kr_clnt_cookie_alg(kr_clnt_cookie_algs,
		                                                                   node->string_);
		if (!cc_alg) {
			return false;
		}
		cntrl->cc_alg = cc_alg;
		return true;
	}

	return false;
}

static bool apply_cache_ttl(struct kr_cookie_ctx *cntrl, const JsonNode *node)
{
	if (node->tag == JSON_NUMBER) {
		cntrl->cache_ttl = node->number_;
		return true;
	}

	return false;
}

static bool apply_configuration(struct kr_cookie_ctx *cntrl, const JsonNode *node)
{
	assert(cntrl && node);

	if (!node->key) {
		/* All top most nodes must have names. */
		return false;
	}

	if (strcmp(node->key, NAME_ENABLED) == 0) {
		return aply_enabled(cntrl, node);
	} else if (strcmp(node->key, NAME_CLIENT_SECRET) == 0) {
		return apply_client_secret(cntrl, node);
	} else  if (strcmp(node->key, NAME_CLIENT_COOKIE_ALG) == 0) {
		return apply_client_hash_func(cntrl, node);
	} else if (strcmp(node->key, NAME_CACHE_TTL) == 0) {
		return apply_cache_ttl(cntrl, node);
	}

	return false;
}

static bool read_secret(JsonNode *root, struct kr_cookie_ctx *cntrl)
{
	assert(root && cntrl);

	JsonNode *array = json_mkarray();
	if (!array) {
		return false;
	}

	for (size_t i = 0; i < cntrl->current_cs->size; ++i) {
		JsonNode *element = json_mknumber(cntrl->current_cs->data[i]);
		if (!element) {
			goto fail;
		}
		json_append_element(array, element);
	}

	json_append_member(root, NAME_CLIENT_SECRET, array);

	return true;

fail:
	if (array) {
		json_delete(array);
	}
	return false;
}

static bool read_available_cc_hashes(JsonNode *root,
                                     struct kr_cookie_ctx *cntrl)
{
	assert(root && cntrl);

	JsonNode *array = json_mkarray();
	if (!array) {
		return false;
	}

	const struct kr_clnt_cookie_alg_descr *aux_ptr = kr_clnt_cookie_algs;
	while (aux_ptr && aux_ptr->func) {
		assert(aux_ptr->name);
		JsonNode *element = json_mkstring(aux_ptr->name);
		if (!element) {
			goto fail;
		}
		json_append_element(array, element);
		++aux_ptr;
	}

	json_append_member(root, NAME_AVAILABLE_CLIENT_COOKIE_ALGS, array);

	return true;

fail:
	if (array) {
		json_delete(array);
	}
	return false;
}

/**
 * Get/set DNS cookie related stuff.
 *
 * Input: { name: value, ... }
 * Output: current configuration
 */
static char *cookiectl_config(void *env, struct kr_module *module, const char *args)
{
	if (args && strlen(args) > 0) {
		JsonNode *node;
		JsonNode *root_node = json_decode(args);
		json_foreach (node, root_node) {
			apply_configuration(&kr_glob_cookie_ctx, node);
		}
		json_delete(root_node);
	}

	/* Return current configuration. */
	char *result = NULL;
	JsonNode *root_node = json_mkobject();

	json_append_member(root_node, NAME_ENABLED,
	                   json_mkbool(kr_glob_cookie_ctx.enabled));

	read_secret(root_node, &kr_glob_cookie_ctx);

	assert(kr_glob_cookie_ctx.cc_alg->name);
	json_append_member(root_node, NAME_CLIENT_COOKIE_ALG,
	                   json_mkstring(kr_glob_cookie_ctx.cc_alg->name));

	read_available_cc_hashes(root_node, &kr_glob_cookie_ctx);

	json_append_member(root_node, NAME_CACHE_TTL,
	                   json_mknumber(kr_glob_cookie_ctx.cache_ttl));

	result = json_encode(root_node);
	json_delete(root_node);
	return result;
}

/*
 * Module implementation.
 */

KR_EXPORT
int cookiectl_init(struct kr_module *module)
{
	struct engine *engine = module->data;

	memset(&kr_glob_cookie_ctx, 0, sizeof(kr_glob_cookie_ctx));

	kr_glob_cookie_ctx.enabled = false;
	kr_glob_cookie_ctx.current_cs = &dflt_cs;
	kr_glob_cookie_ctx.cache_ttl = DFLT_COOKIE_TTL;
	kr_glob_cookie_ctx.current_ss = &dflt_ss;
	kr_glob_cookie_ctx.cc_alg = kr_clnt_cookie_alg(kr_clnt_cookie_algs, "FNV-64");
	kr_glob_cookie_ctx.sc_alg = kr_srvr_cookie_alg(kr_srvr_cookie_algs, "HMAC-SHA256-64");

	module->data = NULL;

	return kr_ok();
}

KR_EXPORT
int cookiectl_deinit(struct kr_module *module)
{
	kr_glob_cookie_ctx.enabled = false;

	if (kr_glob_cookie_ctx.recent_cs &&
	    kr_glob_cookie_ctx.recent_cs != &dflt_cs) {
		free(kr_glob_cookie_ctx.recent_cs);
	}
	kr_glob_cookie_ctx.recent_cs = NULL;

	if (kr_glob_cookie_ctx.current_cs &&
	    kr_glob_cookie_ctx.current_cs != &dflt_cs) {
		free(kr_glob_cookie_ctx.current_cs);
	}
	kr_glob_cookie_ctx.current_cs = &dflt_cs;

	if (kr_glob_cookie_ctx.recent_ss &&
	    kr_glob_cookie_ctx.recent_ss != &dflt_ss) {
		free(kr_glob_cookie_ctx.recent_ss);
	}
	kr_glob_cookie_ctx.recent_ss = NULL;

	if (kr_glob_cookie_ctx.current_ss &&
	    kr_glob_cookie_ctx.current_ss != &dflt_ss) {
		free(kr_glob_cookie_ctx.current_ss);
	}
	kr_glob_cookie_ctx.current_ss = &dflt_ss;

	return kr_ok();
}

KR_EXPORT
struct kr_prop *cookiectl_props(void)
{
	static struct kr_prop prop_list[] = {
	    { &cookiectl_config, "config", "Empty value to return current configuration.", },
	    { NULL, NULL, NULL }
	};
	return prop_list;
}

KR_MODULE_EXPORT(cookiectl);
