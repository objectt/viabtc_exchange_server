/*
 * Description:
 *     History: yang@haipo.me, 2017/03/16, create
 */

# include "me_config.h"
# include "me_server.h"
# include "me_balance.h"
# include "me_update.h"
# include "me_market.h"
# include "me_trade.h"
# include "me_operlog.h"
# include "me_history.h"
# include "me_message.h"

static rpc_svr *svr;
static dict_t *dict_cache;
static nw_timer cache_timer;

struct cache_val {
    double      time;
    json_t      *result;
};

static int reply_json(nw_ses *ses, rpc_pkg *pkg, const json_t *json)
{
    char *message_data;
    if (settings.debug) {
        message_data = json_dumps(json, JSON_INDENT(4));
    } else {
        message_data = json_dumps(json, 0);
    }
    if (message_data == NULL)
        return -__LINE__;
    log_trace("connection: %s send: %s", nw_sock_human_addr(&ses->peer_addr), message_data);

    rpc_pkg reply;
    memcpy(&reply, pkg, sizeof(reply));
    reply.pkg_type = RPC_PKG_TYPE_REPLY;
    reply.body = message_data;
    reply.body_size = strlen(message_data);
    rpc_send(ses, &reply);
    free(message_data);

    return 0;
}

static int reply_error(nw_ses *ses, rpc_pkg *pkg, int code, const char *message)
{
    json_t *error = json_object();
    json_object_set_new(error, "code", json_integer(code+5000));
    json_object_set_new(error, "message", json_string(message));

    json_t *reply = json_object();
    json_object_set_new(reply, "error", error);
    json_object_set_new(reply, "result", json_null());
    json_object_set_new(reply, "id", json_integer(pkg->req_id));

    int ret = reply_json(ses, pkg, reply);
    json_decref(reply);

    return ret;
}

static int reply_error_invalid_argument(nw_ses *ses, rpc_pkg *pkg)
{
    return reply_error(ses, pkg, 1, "invalid argument");
}

static int reply_error_internal_error(nw_ses *ses, rpc_pkg *pkg)
{
    return reply_error(ses, pkg, 2, "internal error");
}

static int reply_error_service_unavailable(nw_ses *ses, rpc_pkg *pkg)
{
    return reply_error(ses, pkg, 3, "service unavailable");
}

static int reply_result(nw_ses *ses, rpc_pkg *pkg, json_t *result)
{
    json_t *reply = json_object();
    json_object_set_new(reply, "error", json_null());
    json_object_set    (reply, "result", result);
    json_object_set_new(reply, "id", json_integer(pkg->req_id));

    int ret = reply_json(ses, pkg, reply);
    json_decref(reply);

    return ret;
}

static int reply_success(nw_ses *ses, rpc_pkg *pkg)
{
    json_t *result = json_object();
    json_object_set_new(result, "status", json_string("success"));

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static bool process_cache(nw_ses *ses, rpc_pkg *pkg, sds *cache_key)
{
    sds key = sdsempty();
    key = sdscatprintf(key, "%u", pkg->command);
    key = sdscatlen(key, pkg->body, pkg->body_size);
    dict_entry *entry = dict_find(dict_cache, key);
    if (entry == NULL) {
        *cache_key = key;
        return false;
    }

    struct cache_val *cache = entry->val;
    double now = current_timestamp();
    if ((now - cache->time) > settings.cache_timeout) {
        dict_delete(dict_cache, key);
        *cache_key = key;
        return false;
    }

    reply_result(ses, pkg, cache->result);
    sdsfree(key);
    return true;
}

static int add_cache(sds cache_key, json_t *result)
{
    struct cache_val cache;
    cache.time = current_timestamp();
    cache.result = result;
    json_incref(result);
    dict_replace(dict_cache, cache_key, &cache);

    return 0;
}

static int on_cmd_balance_query(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    size_t request_size = json_array_size(params);
    if (request_size < 1)
        return reply_error_invalid_argument(ses, pkg);

    // User ID
    if (!json_is_integer(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t user_id = json_integer_value(json_array_get(params, 0));
    if (user_id == 0)
        return reply_error_invalid_argument(ses, pkg);

    json_t *result = json_object();

    // Assets - Show All
    if (request_size == 1) {
        for (size_t i = 0; i < settings.asset_num; ++i) {
            const char *asset = settings.assets[i].name;
            json_t *unit = json_object();
            int prec_save = asset_prec(asset);
            int prec_show = asset_prec_show(asset);

            mpd_t *available = balance_get(user_id, BALANCE_TYPE_AVAILABLE, asset);
            mpd_t *freeze = balance_get(user_id, BALANCE_TYPE_FREEZE, asset);

            if (!available && !freeze)
                continue;

            if (available) {
                if (prec_save != prec_show) {
                    mpd_t *show = mpd_qncopy(available);
                    mpd_rescale(show, show, -prec_show, &mpd_ctx);
                    json_object_set_new_mpd(unit, "available", show);
                    mpd_del(show);
                } else {
                    json_object_set_new_mpd(unit, "available", available);
                }
            } else {
                available = mpd_qncopy(mpd_zero);
                json_object_set_new(unit, "available", json_string("0"));
            }

            if (freeze) {
                if (prec_save != prec_show) {
                    mpd_t *show = mpd_qncopy(freeze);
                    mpd_rescale(show, show, -prec_show, &mpd_ctx);
                    json_object_set_new_mpd(unit, "freeze", show);
                    mpd_del(show);
                } else {
                    json_object_set_new_mpd(unit, "freeze", freeze);
                }
            } else {
                freeze = mpd_qncopy(mpd_zero);
                json_object_set_new(unit, "freeze", json_string("0"));
            }

            // Additional Fields
            mpd_t *total = mpd_new(&mpd_ctx);
            mpd_add(total, available, freeze, &mpd_ctx);
            json_object_set_new_mpd(unit, "total", total);  // Total = available + freeze

            market_t *m = get_market(asset);
            if (m != NULL) {
                mpd_mul(total, total, m->last_price, &mpd_ctx);
                mpd_rescale(total, total, -prec_show, &mpd_ctx);
                json_object_set_new_mpd(unit, "value", total); // Value in default currency
                json_object_set_new_mpd(unit, "last_price", m->last_price);
                json_object_set_new_mpd(unit, "closing_price", m->closing_price);
            }
            mpd_del(total);

            json_object_set_new(result, asset, unit);
        }
    } else {
    // Assets - Show requested assets only
        for (size_t i = 1; i < request_size; ++i) {
            const char *asset = json_string_value(json_array_get(params, i));
            if (!asset || !asset_exist(asset)) {
                json_decref(result);
                return reply_error_invalid_argument(ses, pkg);
            }
            json_t *unit = json_object();
            int prec_save = asset_prec(asset);
            int prec_show = asset_prec_show(asset);

            mpd_t *available = balance_get(user_id, BALANCE_TYPE_AVAILABLE, asset);
            if (available) {
                if (prec_save != prec_show) {
                    mpd_t *show = mpd_qncopy(available);
                    mpd_rescale(show, show, -prec_show, &mpd_ctx);
                    json_object_set_new_mpd(unit, "available", show);
                    mpd_del(show);
                } else {
                    json_object_set_new_mpd(unit, "available", available);
                }
            } else {
                json_object_set_new(unit, "available", json_string("0"));
            }

            mpd_t *freeze = balance_get(user_id, BALANCE_TYPE_FREEZE, asset);
            if (freeze) {
                if (prec_save != prec_show) {
                    mpd_t *show = mpd_qncopy(freeze);
                    mpd_rescale(show, show, -prec_show, &mpd_ctx);
                    json_object_set_new_mpd(unit, "freeze", show);
                    mpd_del(show);
                } else {
                    json_object_set_new_mpd(unit, "freeze", freeze);
                }
            } else {
                json_object_set_new(unit, "freeze", json_string("0"));
            }

            mpd_t *total = mpd_qncopy(mpd_zero);
            if (available)
                mpd_add(total, total, available, &mpd_ctx);
            if (freeze)
                mpd_add(total, total, freeze, &mpd_ctx);
            json_object_set_new_mpd(unit, "total", total);  // Total = available + freeze

            market_t *m = get_market(asset);
            if (m != NULL) {
                mpd_mul(total, total, m->last_price, &mpd_ctx);
                mpd_rescale(total, total, -prec_show, &mpd_ctx);
                json_object_set_new_mpd(unit, "value", total); // Value in default currency
                json_object_set_new_mpd(unit, "last_price", m->last_price);
                json_object_set_new_mpd(unit, "closing_price", m->closing_price);
            }
            mpd_del(total);

            json_object_set_new(result, asset, unit);
        }
    }

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static int on_cmd_balance_update(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 6)
        return reply_error_invalid_argument(ses, pkg);

    // user_id
    if (!json_is_integer(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t user_id = json_integer_value(json_array_get(params, 0));

    // asset
    if (!json_is_string(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    const char *asset = json_string_value(json_array_get(params, 1));
    int prec = asset_prec_show(asset);
    if (prec < 0)
        return reply_error_invalid_argument(ses, pkg);

    // business - freeze/deposit/withdraw
    if (!json_is_string(json_array_get(params, 2)))
        return reply_error_invalid_argument(ses, pkg);
    const char *business = json_string_value(json_array_get(params, 2));

    // business_id
    if (!json_is_integer(json_array_get(params, 3)))
        return reply_error_invalid_argument(ses, pkg);
    uint64_t business_id = json_integer_value(json_array_get(params, 3));

    // change
    if (!json_is_string(json_array_get(params, 4)))
        return reply_error_invalid_argument(ses, pkg);
    mpd_t *change = decimal(json_string_value(json_array_get(params, 4)), prec);
    if (change == NULL)
        return reply_error_invalid_argument(ses, pkg);

    // detail
    json_t *detail = json_array_get(params, 5);
    if (!json_is_object(detail)) {
        mpd_del(change);
        return reply_error_invalid_argument(ses, pkg);
    }

    int ret = update_user_balance(true, user_id, asset, business, business_id, change, detail);
    mpd_del(change);
    if (ret == -1) {
        return reply_error(ses, pkg, 10, "repeat update");
    } else if (ret == -2) {
        return reply_error(ses, pkg, 11, "balance not enough");
    } else if (ret < 0) {
        return reply_error_internal_error(ses, pkg);
    }

    append_operlog("update_balance", params);
    return reply_success(ses, pkg);
}

static int on_cmd_asset_list(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    json_t *result = json_array();
    for (int i = 0; i < settings.asset_num; ++i) {
        json_t *asset = json_object();
        json_object_set_new(asset, "name", json_string(settings.assets[i].name));
        json_object_set_new(asset, "prec", json_integer(settings.assets[i].prec_show));
        json_array_append_new(result, asset);
    }

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static json_t *get_asset_summary(const char *name)
{
    size_t available_count;
    size_t freeze_count;
    size_t total_count;
    mpd_t *total = mpd_new(&mpd_ctx);
    mpd_t *available = mpd_new(&mpd_ctx);
    mpd_t *freeze = mpd_new(&mpd_ctx);
    balance_status(name, total, &available_count, available, &freeze_count, freeze, &total_count);

    json_t *obj = json_object();
    json_object_set_new(obj, "name", json_string(name));
    json_object_set_new_mpd(obj, "total_balance", total);
    json_object_set_new(obj, "available_count", json_integer(available_count));
    json_object_set_new_mpd(obj, "available_balance", available);
    json_object_set_new(obj, "freeze_count", json_integer(freeze_count));
    json_object_set_new_mpd(obj, "freeze_balance", freeze);
    json_object_set_new(obj, "total_count", json_integer(total_count));

    mpd_del(total);
    mpd_del(available);
    mpd_del(freeze);

    return obj;
}

static int on_cmd_asset_summary(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    json_t *result = json_array();
    if (json_array_size(params) == 0) {
        for (int i = 0; i < settings.asset_num; ++i) {
            json_array_append_new(result, get_asset_summary(settings.assets[i].name));
        }
    } else {
        for (int i = 0; i < json_array_size(params); ++i) {
            const char *asset = json_string_value(json_array_get(params, i));
            if (asset == NULL)
                goto invalid_argument;
            if (!asset_exist(asset))
                goto invalid_argument;
            json_array_append_new(result, get_asset_summary(asset));
        }
    }

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;

invalid_argument:
    json_decref(result);
    return reply_error_invalid_argument(ses, pkg);
}

static bool check_makers_exist(uint32_t side, market_t *market)
{
    skiplist_t *list;

    if (side == MARKET_ORDER_SIDE_ASK)
        list = market->bids;
    else
        list = market->asks;

    skiplist_iter *iter = skiplist_get_iterator(list);
    skiplist_node *node = skiplist_next(iter);
    if (node == NULL) {
        skiplist_release_iterator(iter);
        return false;
    }
    skiplist_release_iterator(iter);

    return true;
}

static int on_cmd_order_put(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) > 8 || json_array_size(params) < 6)
        return reply_error_invalid_argument(ses, pkg);

    bool is_price_setter = (pkg->command != CMD_ORDER_PUT_MARKET);
    bool is_maker_candidate = (pkg->command != CMD_ORDER_PUT_MARKET) &&
                             (pkg->command != CMD_ORDER_PUT_FOK);

    // LIMIT, AON (8)
    if (is_maker_candidate && json_array_size(params) != 8)
        return reply_error_invalid_argument(ses, pkg);
    else if (pkg->command == CMD_ORDER_PUT_MARKET
        && json_array_size(params) != 6)
        return reply_error_invalid_argument(ses, pkg);
    else if (pkg->command == CMD_ORDER_PUT_FOK
        && json_array_size(params) != 7)
        return reply_error_invalid_argument(ses, pkg);

    // Argument validation
    int idx = 0;

    // user_id
    if (!json_is_integer(json_array_get(params, idx)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t user_id = json_integer_value(json_array_get(params, idx++));

    // market
    if (!json_is_string(json_array_get(params, idx)))
        return reply_error_invalid_argument(ses, pkg);
    const char *market_name = json_string_value(json_array_get(params, idx++));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return reply_error_invalid_argument(ses, pkg);

    // side
    if (!json_is_integer(json_array_get(params, idx)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t side = json_integer_value(json_array_get(params, idx++));
    if (side != MARKET_ORDER_SIDE_ASK && side != MARKET_ORDER_SIDE_BID)
        return reply_error_invalid_argument(ses, pkg);

    mpd_t *amount = NULL;
    mpd_t *price = mpd_qncopy(mpd_zero);
    mpd_t *taker_fee = NULL;
    mpd_t *maker_fee = mpd_qncopy(mpd_zero);
    mpd_t *rem = mpd_new(&mpd_ctx);
    json_t *result = NULL;

    // amount check
    if (!json_is_string(json_array_get(params, idx)))
        goto invalid_argument;
    amount = decimal(json_string_value(json_array_get(params, idx++)), market->stock_prec);
    if (amount == NULL || mpd_cmp(amount, mpd_zero, &mpd_ctx) <= 0)
        goto invalid_argument;

    // price - Non-Market
    if (is_price_setter) {
        if (!json_is_string(json_array_get(params, idx)))
            goto invalid_argument;
        mpd_del(price);
        price = decimal(json_string_value(json_array_get(params, idx++)), market->money_prec);
        if (price == NULL || mpd_cmp(price, mpd_zero, &mpd_ctx) <= 0)
            goto invalid_argument;
    }

    // taker fee
    if (!json_is_string(json_array_get(params, idx)))
        goto invalid_argument;
    taker_fee = decimal(json_string_value(json_array_get(params, idx++)), market->fee_prec);
    if (taker_fee == NULL || mpd_cmp(taker_fee, mpd_zero, &mpd_ctx) < 0 ||
        mpd_cmp(taker_fee, mpd_one, &mpd_ctx) >= 0)
        goto invalid_argument;

    // maker fee - LIMIT, AON
    if (is_maker_candidate) {
        if (!json_is_string(json_array_get(params, idx)))
            goto invalid_argument;
        mpd_del(maker_fee);
        maker_fee = decimal(json_string_value(json_array_get(params, idx++)), market->fee_prec);
        if (maker_fee == NULL || mpd_cmp(maker_fee, mpd_zero, &mpd_ctx) < 0 ||
            mpd_cmp(maker_fee, mpd_one, &mpd_ctx) >= 0)
            goto invalid_argument;
    }

    if (json_array_size(params) != idx + 1)
        goto invalid_argument;

    // source
    if (!json_is_string(json_array_get(params, idx)))
        goto invalid_argument;
    const char *source = json_string_value(json_array_get(params, idx++));
    if (strlen(source) >= SOURCE_MAX_LEN)
        goto invalid_argument;

    int ret;
    char *oper;
    
    // 2) Amount check
    // 2-1) minimum stock amount
    if (mpd_cmp(amount, market->min_amount, &mpd_ctx) < 0) {
        ret = -2;
        goto invalid_order;
    }
    // 2-2 multiple of stock tick size
    mpd_rem(rem, amount, asset_tick_size(market->stock), &mpd_ctx);
    if (mpd_cmp(rem, mpd_zero, &mpd_ctx) != 0) {
        ret = -2;
        goto invalid_order;
    }

    // 3) Price check
    if (is_price_setter) {
        mpd_t *total = mpd_new(&mpd_ctx);

        // 3-1) multiple of price tick size
        mpd_rem(rem, price, asset_tick_size(market->money), &mpd_ctx);
        if (mpd_cmp(rem, mpd_zero, &mpd_ctx) != 0) {
            ret = -3;
            goto invalid_order;
        }

        // 3-2) price limits
        mpd_mul(total, price, amount, &mpd_ctx);
        mpd_rescale(total, total, -asset_prec(market->money), &mpd_ctx);
        if (mpd_cmp(total, market->min_total, &mpd_ctx) < 0
            || !check_price_limit(market->last_price, price, settings.last_price_limit)
            || !check_price_limit(market->closing_price, price, settings.closing_price_limit)) {
            ret = -4;
            mpd_del(total);
            goto invalid_order;
        }

        mpd_del(total);
    }

    // 4) Balance check
    if (!is_maker_candidate && !check_makers_exist(side, market)) {
        ret = -6;
        goto invalid_order;
    }

    if (side == MARKET_ORDER_SIDE_ASK) {
        mpd_t *balance = balance_get(user_id, BALANCE_TYPE_AVAILABLE, market->stock);
        if (!balance || mpd_cmp(balance, amount, &mpd_ctx) < 0) {
            ret = -1;
            goto invalid_order;
        }
    }
    else if (is_price_setter) {
        mpd_t *balance = balance_get(user_id, BALANCE_TYPE_AVAILABLE, market->money);
        mpd_t *required = mpd_new(&mpd_ctx);
        mpd_mul(required, amount, price, &mpd_ctx);

        if (!balance || mpd_cmp(balance, required, &mpd_ctx) < 0) {
            mpd_del(required);
            ret = -1;
            goto invalid_order;
        }

        if (market->include_fee) {
            mpd_t *max_fee = mpd_new(&mpd_ctx);
            mpd_mul(max_fee, required, taker_fee, &mpd_ctx);
            mpd_add(required, required, max_fee, &mpd_ctx);
            mpd_del(max_fee);

            if (mpd_cmp(balance, required, &mpd_ctx) < 0) {
                mpd_del(required);
                ret = -5;
                goto invalid_order;
            }
        }

        mpd_del(required);
    }
    else {  // MARKET BID
        skiplist_iter *iter = skiplist_get_iterator(market->asks);
        skiplist_node *node = skiplist_next(iter);
        skiplist_release_iterator(iter);

        order_t *order = node->value;
        mpd_t *required = mpd_new(&mpd_ctx);
        mpd_mul(required, order->price, amount, &mpd_ctx);
        if (mpd_cmp(required, market->min_total, &mpd_ctx) < 0) {
            mpd_del(required);
            ret = -4;
            goto invalid_order;
        }
        mpd_del(required);
    }

    switch(pkg->command) {
        case CMD_ORDER_PUT_LIMIT:
            ret = market_put_limit_order(true, &result, market, user_id, side,
                                   amount, price, taker_fee, maker_fee, source);
            oper = "limit_order";
            break;
        case CMD_ORDER_PUT_AON:
            ret = market_put_aon_order(true, &result, market, user_id, side,
                                   amount, price, taker_fee, maker_fee, source);
            oper = "aon_order";
            break;
        case CMD_ORDER_PUT_MARKET:
            ret = market_put_market_order(true, &result, market, user_id, side,
                                                     amount, taker_fee, source);
            oper = "market_order";
            break;
        case CMD_ORDER_PUT_FOK:
            ret = market_put_fok_order(true, &result, market, user_id, side,
                                              amount, price, taker_fee, source);
            oper = "fok_order";
            break;
        default:
            goto invalid_argument;
    }

invalid_order:
    mpd_del(amount);
    mpd_del(price);
    mpd_del(taker_fee);
    mpd_del(maker_fee);
    mpd_del(rem);

    if (ret == -1)
        return reply_error(ses, pkg, 10, "insufficient balance");
    else if (ret == -2)
        return reply_error(ses, pkg, 11, "invalid amount");
    else if (ret == -3)
        return reply_error(ses, pkg, 14, "invalid price");
    else if (ret == -4)
        return reply_error(ses, pkg, 12, "price out of range");
    else if (ret == -5)
        return reply_error(ses, pkg, 13, "insufficient trading fee");
    else if (ret == -6)
        return reply_error(ses, pkg, 15, "no orders found");
    else if (ret < 0) {
        log_fatal("market_put_limit_order fail: %d", ret);
        return reply_error_internal_error(ses, pkg);
    }

    append_operlog(oper, params);
    ret = reply_result(ses, pkg, result);
    json_decref(result);

    return ret;

invalid_argument:
    if (amount)
        mpd_del(amount);
    if (price)
        mpd_del(price);
    if (taker_fee)
        mpd_del(taker_fee);
    if (maker_fee)
        mpd_del(maker_fee);
    if (rem)
        mpd_del(rem);
    if (result)
        json_decref(result);

    return reply_error_invalid_argument(ses, pkg);
}

static int on_cmd_order_query(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 4)
        return reply_error_invalid_argument(ses, pkg);

    // user_id
    if (!json_is_integer(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t user_id = json_integer_value(json_array_get(params, 0));

    // market
    if (!json_is_string(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    const char *market_name = json_string_value(json_array_get(params, 1));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return reply_error_invalid_argument(ses, pkg);

    // offset
    if (!json_is_integer(json_array_get(params, 2)))
        return reply_error_invalid_argument(ses, pkg);
    size_t offset = json_integer_value(json_array_get(params, 2));

    // limit
    if (!json_is_integer(json_array_get(params, 3)))
        return reply_error_invalid_argument(ses, pkg);
    size_t limit = json_integer_value(json_array_get(params, 3));
    if (limit > ORDER_LIST_MAX_LEN)
        return reply_error_invalid_argument(ses, pkg);

    json_t *result = json_object();
    json_object_set_new(result, "limit", json_integer(limit));
    json_object_set_new(result, "offset", json_integer(offset));

    json_t *orders = json_array();
    skiplist_t *order_list = market_get_order_list(market, user_id);
    if (order_list == NULL) {
        json_object_set_new(result, "total", json_integer(0));
    } else {
        json_object_set_new(result, "total", json_integer(order_list->len));
        if (offset < order_list->len) {
            skiplist_iter *iter = skiplist_get_iterator(order_list);
            skiplist_node *node;
            for (size_t i = 0; i < offset; i++) {
                if (skiplist_next(iter) == NULL)
                    break;
            }
            size_t index = 0;
            while ((node = skiplist_next(iter)) != NULL && index < limit) {
                index++;
                order_t *order = node->value;
                json_array_append_new(orders, get_order_info(order));
            }
            skiplist_release_iterator(iter);
        }
    }

    json_object_set_new(result, "records", orders);
    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static int on_cmd_order_cancel(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 3)
        return reply_error_invalid_argument(ses, pkg);

    // user_id
    if (!json_is_integer(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t user_id = json_integer_value(json_array_get(params, 0));

    // market
    if (!json_is_string(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    const char *market_name = json_string_value(json_array_get(params, 1));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return reply_error_invalid_argument(ses, pkg);

    // order_id
    if (!json_is_integer(json_array_get(params, 2)))
        return reply_error_invalid_argument(ses, pkg);
    uint64_t order_id = json_integer_value(json_array_get(params, 2));

    order_t *order = market_get_order(market, order_id);
    if (order == NULL) {
        return reply_error(ses, pkg, 10, "order not found");
    }
    if (order->user_id != user_id) {
        return reply_error(ses, pkg, 11, "user mismatch");
    }

    json_t *result = NULL;
    int ret = market_cancel_order(true, &result, market, order);
    if (ret < 0) {
        log_fatal("cancel order: %"PRIu64" fail: %d", order->id, ret);
        return reply_error_internal_error(ses, pkg);
    }

    append_operlog("cancel_order", params);
    ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static int on_cmd_order_book(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 4)
        return reply_error_invalid_argument(ses, pkg);

    // market
    if (!json_is_string(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    const char *market_name = json_string_value(json_array_get(params, 0));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return reply_error_invalid_argument(ses, pkg);

    // side
    if (!json_is_integer(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t side = json_integer_value(json_array_get(params, 1));
    if (side != MARKET_ORDER_SIDE_ASK && side != MARKET_ORDER_SIDE_BID)
        return reply_error_invalid_argument(ses, pkg);

    // offset
    if (!json_is_integer(json_array_get(params, 2)))
        return reply_error_invalid_argument(ses, pkg);
    size_t offset = json_integer_value(json_array_get(params, 2));

    // limit
    if (!json_is_integer(json_array_get(params, 3)))
        return reply_error_invalid_argument(ses, pkg);
    size_t limit = json_integer_value(json_array_get(params, 3));
    if (limit > ORDER_BOOK_MAX_LEN)
        return reply_error_invalid_argument(ses, pkg);

    json_t *result = json_object();
    json_object_set_new(result, "offset", json_integer(offset));
    json_object_set_new(result, "limit", json_integer(limit));

    uint64_t total;
    skiplist_iter *iter;
    if (side == MARKET_ORDER_SIDE_ASK) {
        iter = skiplist_get_iterator(market->asks);
        total = market->asks->len;
        json_object_set_new(result, "total", json_integer(total));
    } else {
        iter = skiplist_get_iterator(market->bids);
        total = market->bids->len;
        json_object_set_new(result, "total", json_integer(total));
    }

    json_t *orders = json_array();
    if (offset < total) {
        for (size_t i = 0; i < offset; i++) {
            if (skiplist_next(iter) == NULL)
                break;
        }
        size_t index = 0;
        skiplist_node *node;
        while ((node = skiplist_next(iter)) != NULL && index < limit) {
            index++;
            order_t *order = node->value;
            json_array_append_new(orders, get_order_info(order));
        }
    }
    skiplist_release_iterator(iter);

    json_object_set_new(result, "orders", orders);
    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static json_t *get_depth(market_t *market, size_t limit)
{
    mpd_t *price = mpd_new(&mpd_ctx);
    mpd_t *amount = mpd_new(&mpd_ctx);

    json_t *asks = json_array();
    skiplist_iter *iter = skiplist_get_iterator(market->asks);
    skiplist_node *node = skiplist_next(iter);
    size_t index = 0;
    while (node && index < limit) {
        index++;
        order_t *order = node->value;
        mpd_copy(price, order->price, &mpd_ctx);
        mpd_copy(amount, order->left, &mpd_ctx);
        while ((node = skiplist_next(iter)) != NULL) {
            order = node->value;
            if (mpd_cmp(price, order->price, &mpd_ctx) == 0) {
                mpd_add(amount, amount, order->left, &mpd_ctx);
            } else {
                break;
            }
        }
        json_t *info = json_array();
        json_array_append_new_mpd(info, price);
        json_array_append_new_mpd(info, amount);
        json_array_append_new(asks, info);
    }
    skiplist_release_iterator(iter);

    json_t *bids = json_array();
    iter = skiplist_get_iterator(market->bids);
    node = skiplist_next(iter);
    index = 0;
    while (node && index < limit) {
        index++;
        order_t *order = node->value;
        mpd_copy(price, order->price, &mpd_ctx);
        mpd_copy(amount, order->left, &mpd_ctx);
        while ((node = skiplist_next(iter)) != NULL) {
            order = node->value;
            if (mpd_cmp(price, order->price, &mpd_ctx) == 0) {
                mpd_add(amount, amount, order->left, &mpd_ctx);
            } else {
                break;
            }
        }
        json_t *info = json_array();
        json_array_append_new_mpd(info, price);
        json_array_append_new_mpd(info, amount);
        json_array_append_new(bids, info);
    }
    skiplist_release_iterator(iter);

    mpd_del(price);
    mpd_del(amount);

    json_t *result = json_object();
    json_object_set_new(result, "asks", asks);
    json_object_set_new(result, "bids", bids);

    return result;
}

static json_t *get_depth_merge(market_t* market, size_t limit, mpd_t *interval)
{
    mpd_t *q = mpd_new(&mpd_ctx);
    mpd_t *r = mpd_new(&mpd_ctx);
    mpd_t *price = mpd_new(&mpd_ctx);
    mpd_t *amount = mpd_new(&mpd_ctx);

    json_t *asks = json_array();
    skiplist_iter *iter = skiplist_get_iterator(market->asks);
    skiplist_node *node = skiplist_next(iter);
    size_t index = 0;
    while (node && index < limit) {
        index++;
        order_t *order = node->value;
        mpd_divmod(q, r, order->price, interval, &mpd_ctx);
        mpd_mul(price, q, interval, &mpd_ctx);
        if (mpd_cmp(r, mpd_zero, &mpd_ctx) != 0) {
            mpd_add(price, price, interval, &mpd_ctx);
        }
        mpd_copy(amount, order->left, &mpd_ctx);
        while ((node = skiplist_next(iter)) != NULL) {
            order = node->value;
            if (mpd_cmp(price, order->price, &mpd_ctx) >= 0) {
                mpd_add(amount, amount, order->left, &mpd_ctx);
            } else {
                break;
            }
        }
        json_t *info = json_array();
        json_array_append_new_mpd(info, price);
        json_array_append_new_mpd(info, amount);
        json_array_append_new(asks, info);
    }
    skiplist_release_iterator(iter);

    json_t *bids = json_array();
    iter = skiplist_get_iterator(market->bids);
    node = skiplist_next(iter);
    index = 0;
    while (node && index < limit) {
        index++;
        order_t *order = node->value;
        mpd_divmod(q, r, order->price, interval, &mpd_ctx);
        mpd_mul(price, q, interval, &mpd_ctx);
        mpd_copy(amount, order->left, &mpd_ctx);
        while ((node = skiplist_next(iter)) != NULL) {
            order = node->value;
            if (mpd_cmp(price, order->price, &mpd_ctx) <= 0) {
                mpd_add(amount, amount, order->left, &mpd_ctx);
            } else {
                break;
            }
        }

        json_t *info = json_array();
        json_array_append_new_mpd(info, price);
        json_array_append_new_mpd(info, amount);
        json_array_append_new(bids, info);
    }
    skiplist_release_iterator(iter);

    mpd_del(q);
    mpd_del(r);
    mpd_del(price);
    mpd_del(amount);

    json_t *result = json_object();
    json_object_set_new(result, "asks", asks);
    json_object_set_new(result, "bids", bids);

    return result;
}

static int on_cmd_order_book_depth(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 3)
        return reply_error_invalid_argument(ses, pkg);

    // market
    if (!json_is_string(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    const char *market_name = json_string_value(json_array_get(params, 0));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return reply_error_invalid_argument(ses, pkg);

    // limit
    if (!json_is_integer(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    size_t limit = json_integer_value(json_array_get(params, 1));
    if (limit > ORDER_BOOK_MAX_LEN)
        return reply_error_invalid_argument(ses, pkg);

    // interval
    if (!json_is_string(json_array_get(params, 2)))
        return reply_error_invalid_argument(ses, pkg);
    mpd_t *interval = decimal(json_string_value(json_array_get(params, 2)), market->money_prec);
    if (!interval)
        return reply_error_invalid_argument(ses, pkg);
    if (mpd_cmp(interval, mpd_zero, &mpd_ctx) < 0) {
        mpd_del(interval);
        return reply_error_invalid_argument(ses, pkg);
    }

    sds cache_key = NULL;
    if (process_cache(ses, pkg, &cache_key)) {
        mpd_del(interval);
        return 0;
    }

    json_t *result = NULL;
    if (mpd_cmp(interval, mpd_zero, &mpd_ctx) == 0) {
        result = get_depth(market, limit);
    } else {
        result = get_depth_merge(market, limit, interval);
    }
    mpd_del(interval);

    if (result == NULL) {
        sdsfree(cache_key);
        return reply_error_internal_error(ses, pkg);
    }

    add_cache(cache_key, result);
    sdsfree(cache_key);

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static int on_cmd_order_detail(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 2)
        return reply_error_invalid_argument(ses, pkg);

    // market
    if (!json_is_string(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    const char *market_name = json_string_value(json_array_get(params, 0));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return reply_error_invalid_argument(ses, pkg);

    // order_id
    if (!json_is_integer(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    uint64_t order_id = json_integer_value(json_array_get(params, 1));

    order_t *order = market_get_order(market, order_id);
    json_t *result = NULL;
    if (order == NULL) {
        result = json_null();
    } else {
        result = get_order_info(order);
    }

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static int on_cmd_market_list(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    json_t *result = json_array();
    for (int i = 0; i < settings.market_num; ++i) {
        market_t *m = get_market(settings.markets[i].name);

        json_t *market = json_object();
        json_object_set_new(market, "symbol", json_string(settings.markets[i].name));
        json_object_set_new(market, "name", json_string(settings.markets[i].name_full));
        json_object_set_new(market, "base", json_string(settings.markets[i].stock));
        json_object_set_new(market, "counter", json_string(settings.markets[i].money));
        json_object_set_new(market, "fee_prec", json_integer(settings.markets[i].fee_prec));
        json_object_set_new(market, "stock_prec", json_integer(settings.markets[i].stock_prec));
        json_object_set_new(market, "money_prec", json_integer(settings.markets[i].money_prec));
        json_object_set_new(market, "delisting_ts", json_integer(settings.markets[i].delisting_ts));
        json_object_set_new_mpd(market, "min_total", settings.markets[i].min_total);
        json_object_set_new_mpd(market, "init_price", settings.markets[i].init_price);
        json_object_set_new_mpd(market, "closing_price", m->closing_price);
        json_object_set_new_mpd(market, "last_price", m->last_price);
        json_array_append_new(result, market);
    }

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static json_t *get_market_summary(const char *name)
{
    size_t ask_count;
    size_t bid_count;
    mpd_t *ask_amount = mpd_new(&mpd_ctx);
    mpd_t *bid_amount = mpd_new(&mpd_ctx);
    market_t *market = get_market(name);
    market_get_status(market, &ask_count, ask_amount, &bid_count, bid_amount);

    json_t *obj = json_object();
    json_object_set_new(obj, "name", json_string(name));
    json_object_set_new(obj, "ask_count", json_integer(ask_count));
    json_object_set_new_mpd(obj, "ask_amount", ask_amount);
    json_object_set_new(obj, "bid_count", json_integer(bid_count));
    json_object_set_new_mpd(obj, "bid_amount", bid_amount);

    mpd_del(ask_amount);
    mpd_del(bid_amount);

    return obj;
}

static int on_cmd_market_summary(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    json_t *result = json_array();
    if (json_array_size(params) == 0) {
        for (int i = 0; i < settings.market_num; ++i) {
            json_array_append_new(result, get_market_summary(settings.markets[i].name));
        }
    } else {
        for (int i = 0; i < json_array_size(params); ++i) {
            const char *market = json_string_value(json_array_get(params, i));
            if (market == NULL)
                goto invalid_argument;
            if (get_market(market) == NULL)
                goto invalid_argument;
            json_array_append_new(result, get_market_summary(market));
        }
    }

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;

invalid_argument:
    json_decref(result);
    return reply_error_invalid_argument(ses, pkg);
}

static int on_cmd_asset_register(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 3)
        return reply_error_invalid_argument(ses, pkg);

    if (!json_is_string(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    const char *symbol = json_string_value(json_array_get(params, 0));
    if (asset_exist(symbol))
        return reply_error_invalid_argument(ses, pkg);

    if (!json_is_string(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    const char *name = json_string_value(json_array_get(params, 1));
    if (name == NULL)
        return reply_error_invalid_argument(ses, pkg);

    mpd_t *tick_size = decimal(json_string_value(json_array_get(params, 2)), 8);
    if (tick_size == NULL || mpd_cmp(tick_size, mpd_zero, &mpd_ctx) <= 0)
        return reply_error_invalid_argument(ses, pkg);

    char *tick_size_str = mpd_to_sci(tick_size, 0);
    mpd_del(tick_size);
    if (asset_register(symbol, name, tick_size_str) < 0) {
        free(tick_size_str);
        return reply_error_internal_error(ses, pkg);
    }

    free(tick_size_str);
    return reply_success(ses, pkg);
}

static int on_cmd_market_register(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 6)
        return reply_error_invalid_argument(ses, pkg);

    if (!json_is_string(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    const char *ticker = json_string_value(json_array_get(params, 0));
    if (get_market(ticker))
        return reply_error_invalid_argument(ses, pkg);

    if (!json_is_string(json_array_get(params, 1)))
        return reply_error_invalid_argument(ses, pkg);
    const char *name = json_string_value(json_array_get(params, 1));
    if (name == NULL)
        return reply_error_invalid_argument(ses, pkg);

    if (!json_is_string(json_array_get(params, 2)))
        return reply_error_invalid_argument(ses, pkg);
    const char *base = json_string_value(json_array_get(params, 2));
    int base_id = asset_id(base);
    if (base_id < 0)
        return reply_error_invalid_argument(ses, pkg);

    if (!json_is_string(json_array_get(params, 3)))
        return reply_error_invalid_argument(ses, pkg);
    const char *counter = json_string_value(json_array_get(params, 3));
    int counter_id = asset_id(counter);
    if (counter_id < 0)
        return reply_error_invalid_argument(ses, pkg);

    // minimum total
    // minimum amount
    // minimum price
    // initial price

    mpd_t *init_price = mpd_qncopy(mpd_zero);
    if (!json_is_string(json_array_get(params, 4)))
        return reply_error_invalid_argument(ses, pkg);
    mpd_del(init_price);
    init_price = decimal(json_string_value(json_array_get(params, 4)), 8);
    if (init_price == NULL) {
        mpd_del(init_price);
        return reply_error_invalid_argument(ses, pkg);
    }

    char *init_price_str = mpd_to_sci(init_price, 0);
    mpd_del(init_price);

    if (!json_is_integer(json_array_get(params, 5)))
        return reply_error_invalid_argument(ses, pkg);
    uint32_t delisting_ts = json_integer_value(json_array_get(params, 5));

    if (market_register(ticker, name, base_id, counter_id, init_price_str, delisting_ts) < 0) {
        free(init_price_str);
        return reply_error_internal_error(ses, pkg);
    }

    free(init_price_str);
    return reply_success(ses, pkg);
}

static int on_cmd_market_detail(nw_ses *ses, rpc_pkg *pkg, json_t *params)
{
    if (json_array_size(params) != 1)
        return reply_error_invalid_argument(ses, pkg);

    if (!json_is_string(json_array_get(params, 0)))
        return reply_error_invalid_argument(ses, pkg);
    const char *market_name = json_string_value(json_array_get(params, 0));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return reply_error_invalid_argument(ses, pkg);

    json_t *result = market_detail(market);

    int ret = reply_result(ses, pkg, result);
    json_decref(result);
    return ret;
}

static void svr_on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    json_t *params = json_loadb(pkg->body, pkg->body_size, 0, NULL);
    if (params == NULL || !json_is_array(params)) {
        goto decode_error;
    }
    sds params_str = sdsnewlen(pkg->body, pkg->body_size);

    int ret;
    switch (pkg->command) {
    case CMD_BALANCE_QUERY:
        log_trace("from: %s cmd balance query, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_balance_query(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_balance_query %s fail: %d", params_str, ret);
        }
        break;
    case CMD_BALANCE_UPDATE:
        if (is_operlog_block() || is_history_block() || is_message_block() || signal_block) {
            log_fatal("service unavailable, operlog: %d, history: %d, message: %d",
                    is_operlog_block(), is_history_block(), is_message_block());
            reply_error_service_unavailable(ses, pkg);
            goto cleanup;
        }
        log_trace("from: %s cmd balance update, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_balance_update(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_balance_update %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ASSET_LIST:
        log_trace("from: %s cmd asset list, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_asset_list(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_asset_list %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ASSET_SUMMARY:
        log_trace("from: %s cmd asset summary, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_asset_summary(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_asset_summary %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ASSET_REGISTER:
        log_trace("from: %s cmd asset register, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_asset_register(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_asset_register %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ORDER_PUT_LIMIT:
    case CMD_ORDER_PUT_MARKET:
    case CMD_ORDER_PUT_AON:
    case CMD_ORDER_PUT_FOK:
        if (is_operlog_block() || is_history_block() || is_message_block() || signal_block) {
            log_fatal("service unavailable, operlog: %d, history: %d, message: %d",
                    is_operlog_block(), is_history_block(), is_message_block());
            reply_error_service_unavailable(ses, pkg);
            goto cleanup;
        }
        log_trace("from: %s cmd order put, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);

        ret = on_cmd_order_put(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_order_put %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ORDER_QUERY:
        log_trace("from: %s cmd order query, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_order_query(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_order_query %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ORDER_CANCEL:
        if (is_operlog_block() || is_history_block() || is_message_block()) {
            log_fatal("service unavailable, operlog: %d, history: %d, message: %d",
                    is_operlog_block(), is_history_block(), is_message_block());
            reply_error_service_unavailable(ses, pkg);
            goto cleanup;
        }
        log_trace("from: %s cmd order cancel, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_order_cancel(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_order_cancel %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ORDER_BOOK:
        log_trace("from: %s cmd order book, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_order_book(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_order_book %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ORDER_BOOK_DEPTH:
        log_trace("from: %s cmd order book depth, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_order_book_depth(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_order_book_depth %s fail: %d", params_str, ret);
        }
        break;
    case CMD_ORDER_DETAIL:
        log_trace("from: %s cmd order detail, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_order_detail(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_order_detail %s fail: %d", params_str, ret);
        }
        break;
    case CMD_MARKET_LIST:
        log_trace("from: %s cmd market list, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_market_list(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_market_list %s fail: %d", params_str, ret);
        }
        break;
    case CMD_MARKET_SUMMARY:
        log_trace("from: %s cmd market summary, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_market_summary(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_market_summary%s fail: %d", params_str, ret);
        }
        break;
    case CMD_MARKET_REGISTER:
        log_trace("from: %s cmd market register, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_market_register(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_market_register%s fail: %d", params_str, ret);
        }
        break;
    case CMD_MARKET_DETAIL:
        log_trace("from: %s cmd market detail, sequence: %u params: %s", nw_sock_human_addr(&ses->peer_addr), pkg->sequence, params_str);
        ret = on_cmd_market_detail(ses, pkg, params);
        if (ret < 0) {
            log_error("on_cmd_market_detail%s fail: %d", params_str, ret);
        }
        break;
    default:
        log_error("from: %s unknown command: %u", nw_sock_human_addr(&ses->peer_addr), pkg->command);
        break;
    }

cleanup:
    sdsfree(params_str);
    json_decref(params);
    return;

decode_error:
    if (params) {
        json_decref(params);
    }
    sds hex = hexdump(pkg->body, pkg->body_size);
    log_error("connection: %s, cmd: %u decode params fail, params data: \n%s", \
            nw_sock_human_addr(&ses->peer_addr), pkg->command, hex);
    sdsfree(hex);
    rpc_svr_close_clt(svr, ses);

    return;
}

static void svr_on_new_connection(nw_ses *ses)
{
    log_trace("new connection: %s", nw_sock_human_addr(&ses->peer_addr));
}

static void svr_on_connection_close(nw_ses *ses)
{
    log_trace("connection: %s close", nw_sock_human_addr(&ses->peer_addr));
}

static uint32_t cache_dict_hash_function(const void *key)
{
    return dict_generic_hash_function(key, sdslen((sds)key));
}

static int cache_dict_key_compare(const void *key1, const void *key2)
{
    return sdscmp((sds)key1, (sds)key2);
}

static void *cache_dict_key_dup(const void *key)
{
    return sdsdup((const sds)key);
}

static void cache_dict_key_free(void *key)
{
    sdsfree(key);
}

static void *cache_dict_val_dup(const void *val)
{
    struct cache_val *obj = malloc(sizeof(struct cache_val));
    memcpy(obj, val, sizeof(struct cache_val));
    return obj;
}

static void cache_dict_val_free(void *val)
{
    struct cache_val *obj = val;
    json_decref(obj->result);
    free(val);
}

static void on_cache_timer(nw_timer *timer, void *privdata)
{
    dict_clear(dict_cache);
}

int init_server(void)
{
    rpc_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_recv_pkg = svr_on_recv_pkg;
    type.on_new_connection = svr_on_new_connection;
    type.on_connection_close = svr_on_connection_close;

    svr = rpc_svr_create(&settings.svr, &type);
    if (svr == NULL)
        return -__LINE__;
    if (rpc_svr_start(svr) < 0)
        return -__LINE__;

    dict_types dt;
    memset(&dt, 0, sizeof(dt));
    dt.hash_function  = cache_dict_hash_function;
    dt.key_compare    = cache_dict_key_compare;
    dt.key_dup        = cache_dict_key_dup;
    dt.key_destructor = cache_dict_key_free;
    dt.val_dup        = cache_dict_val_dup;
    dt.val_destructor = cache_dict_val_free;

    dict_cache = dict_create(&dt, 64);
    if (dict_cache == NULL)
        return -__LINE__;

    nw_timer_set(&cache_timer, 60, true, on_cache_timer, NULL);
    nw_timer_start(&cache_timer);

    return 0;
}
