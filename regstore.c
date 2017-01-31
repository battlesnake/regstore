#if 0
(
set -euo pipefail
declare -r tmp="$(mktemp)"
gcc -I./c_modules -DSIMPLE_LOGGING -std=gnu11 -DTEST_regstore -g -O0 -Wall -Wextra -Werror $(find -name '*.c') -o "$tmp"
exec valgrind --quiet --leak-check=full --track-origins=yes "$tmp"
)
exit 0
#endif
#include <cstd/std.h>
#include <cstruct/binary_tree_iterator.h>
#include "regstore.h"

const char *regstore_errstr(enum regstore_err error)
{
	switch (error) {
	case regstore_err_ok: return "OK";
	case regstore_err_invalid_key: return "Invalid key";
	case regstore_err_invalid_value: return "Invalid value";
	case regstore_err_unknown: return "Unknown error";
	case regstore_err_not_readable: return "Not readable";
	case regstore_err_not_writeable: return "Not writeable";
	case regstore_err_no_change: return "No change";
	default: return "Unknown error code";
	}
}

/* Register */
struct reg {
	struct fstr name;
	regstore_getter *getter;
	void *getter_arg;
	regstore_setter *setter;
	void *setter_arg;
	struct binary_tree observers; /* observer(remote) */
};

/* Observer */
struct observer {
	struct fstr remote;
	regstore_observer *observer;
	void *observer_arg;
	struct regstore_subscription_info info;
};

static void destroy_reg(void *p, size_t len)
{
	(void) len;
	struct reg *reg = p;
	fstr_destroy(&reg->name);
	binary_tree_destroy(&reg->observers);
}

static void destroy_observer(void *p, size_t len)
{
	(void) len;
	struct observer *obs = p;
	fstr_destroy(&obs->remote);
}

static void destroy_reginfo(void *p, size_t len)
{
	(void) len;
	struct regstore_reginfo *info = p;
	fstr_destroy(&info->name);
	fstr_destroy(&info->value);
}

static enum regstore_err call_getter(const struct reg *reg, struct fstr *value)
{
	if (!reg->getter) {
		return regstore_err_not_readable;
	}
	return reg->getter(reg->setter_arg, value);
}

static enum regstore_err call_setter(const struct reg *reg, const struct fstr *value)
{
	if (!reg->setter) {
		return regstore_err_not_writeable;
	}
	return reg->setter(reg->setter_arg, value);
}

static void call_observer(const struct observer *obs, const struct fstr *value)
{
	obs->observer(obs->observer_arg, value);
}

static void *send_notification_iter(void *arg, struct binary_tree_node *node)
{
	const struct fstr *value = arg;
	struct observer *obs = (void *) node->data;
	/* TODO: Check timestamp in reg info to see whether we should send */
	ssize_t now = 0;
	if (obs->info.next_ms <= now) {
		//TODO obs->info.next_ms = now + obs->info.min_interval_ms;
		call_observer(obs, value);
	}
	return NULL;
}

static void send_notification(const struct reg *reg, const struct fstr *value)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
	binary_tree_each(&reg->observers, send_notification_iter, value);
#pragma GCC diagnostic pop
}

static int first_fstr_cmp(const void *a, size_t al, const void *b, size_t bl, void *arg)
{
	(void) al;
	(void) bl;
	(void) arg;
	const struct fstr *ra = a;
	const struct fstr *rb = b;
	return fstr_cmp(ra, rb);

}

struct list_closure {
	struct binary_tree *out;
	const struct fstr *remote;
	bool values;
};

void *list_iter(void *arg, struct binary_tree_node *node)
{
	const struct list_closure *closure = arg;
	struct binary_tree *out = closure->out;
	const struct fstr *remote = closure->remote;
	bool values = closure->values;

	struct reg *reg = (void *) node->data;

	struct regstore_reginfo info;
	fstr_init_copy(&info.name, &reg->name);
	info.type = (reg->getter ? rt_readable : 0) | (reg->setter ? rt_writeable : 0);
	fstr_init(&info.value);

	if (values && reg->getter) {
		call_getter(reg, &info.value);
	}

	struct observer *obs = binary_tree_get(&reg->observers, remote, sizeof(*remote), NULL);
	info.subscribed = obs != NULL;
	if (obs) {
		info.sub_info = obs->info;
	}

	if (!binary_tree_insert(out, &info, sizeof(info), NULL)) {
		log_error("Unexpected conflict while building register info tree");
		return (void *) 1;
	}
	return NULL;
}

bool regstore_list(struct regstore *inst, struct binary_tree *out, const struct fstr *remote, bool values)
{
	struct list_closure closure = {
		.out = out,
		.remote = remote,
		.values = values
	};
	binary_tree_init(out, first_fstr_cmp, NULL, destroy_reginfo);
	if (binary_tree_each(&inst->store, list_iter, &closure)) {
		binary_tree_destroy(out);
		return false;
	}
	return true;
}

bool regstore_add(struct regstore *inst, const struct fstr *key, regstore_getter *getter, void *getter_arg, regstore_setter *setter, void *setter_arg)
{
	struct reg reg;
	fstr_init_copy(&reg.name, key);
	reg.getter = getter;
	reg.getter_arg = getter_arg;
	reg.setter = setter;
	reg.setter_arg = setter_arg;
	binary_tree_init(&reg.observers, first_fstr_cmp, NULL, destroy_observer);
	if (!binary_tree_insert_new(&inst->store, &reg, sizeof(reg))) {
		fstr_destroy(&reg.name);
		return false;
	}
	return true;
}

bool regstore_add_s(struct regstore *inst, const char *key, regstore_getter *getter, void *getter_arg, regstore_setter *setter, void *setter_arg)
{
	struct fstr fs;
	fstr_init_ref(&fs, key);
	bool res = regstore_add(inst, &fs, getter, getter_arg, setter, setter_arg);
	fstr_destroy(&fs);
	return res;
}

bool regstore_delete(struct regstore *inst, const struct fstr *key)
{
	return binary_tree_remove(&inst->store, key, sizeof(*key));
}

enum regstore_err regstore_set(struct regstore *inst, const struct fstr *key, const struct fstr *value)
{
	struct reg *reg = binary_tree_get(&inst->store, key, sizeof(*key), NULL);
	if (!reg) {
		return regstore_err_invalid_key;
	}
	enum regstore_err err = call_setter(reg, value);
	if (err == regstore_err_ok) {
		struct fstr val;
		fstr_init(&val);
		if (call_getter(reg, &val) == regstore_err_not_readable) {
			send_notification(reg, value);
		} else {
			send_notification(reg, &val);
		}
		fstr_destroy(&val);
	}
	return err == regstore_err_no_change ? regstore_err_ok : err;
}

enum regstore_err regstore_get(struct regstore *inst, const struct fstr *key, struct fstr *value)
{
	struct reg *reg = binary_tree_get(&inst->store, key, sizeof(*key), NULL);
	if (!reg) {
		return regstore_err_invalid_key;
	}
	return call_getter(reg, value);
}

bool regstore_observe(struct regstore *inst, const struct fstr *key, const struct fstr *remote, regstore_observer *observer, void *observer_arg, int64_t min_interval)
{
	if (!observer) {
		return false;
	}
	struct reg *reg = binary_tree_get(&inst->store, key, sizeof(*key), NULL);
	if (!reg) {
		return false;
	}
	struct observer obs;
	fstr_init_copy(&obs.remote, remote);
	obs.observer = observer;
	obs.observer_arg = observer_arg;
	obs.info.next_ms = 0;
	obs.info.min_interval_ms = min_interval;
	binary_tree_replace(&reg->observers, &obs, sizeof(obs));
	return true;
}

bool regstore_unobserve(struct regstore *inst, const struct fstr *key, const struct fstr *remote)
{
	struct reg *reg = binary_tree_get(&inst->store, key, sizeof(*key), NULL);
	if (!reg) {
		return false;
	}
	return binary_tree_remove(&reg->observers, remote, sizeof(*remote));
}

bool regstore_query_observer(struct regstore *inst, const struct fstr *key, const struct fstr *remote, struct regstore_subscription_info *out)
{
	struct reg *reg = binary_tree_get(&inst->store, key, sizeof(*key), NULL);
	if (!reg) {
		return false;
	}
	struct observer *obs = binary_tree_get(&reg->observers, remote, sizeof(*remote), NULL);
	if (!obs) {
		return false;
	}
	*out = obs->info;
	return true;
}

enum regstore_err regstore_notify(struct regstore *inst, const struct fstr *key)
{
	struct reg *reg = binary_tree_get(&inst->store, key, sizeof(*key), NULL);
	if (!reg) {
		return false;
	}
	struct fstr value;
	fstr_init(&value);
	enum regstore_err res = call_getter(reg, &value);
	if (res == regstore_err_ok) {
		send_notification(reg, &value);
	}
	fstr_destroy(&value);
	return res;
}

void regstore_init(struct regstore *inst)
{
	binary_tree_init(&inst->store, first_fstr_cmp, NULL, destroy_reg);
}

void regstore_destroy(struct regstore *inst)
{
	binary_tree_destroy(&inst->store);
}

#if defined TEST_regstore
static struct fstr rem;

#define nregs 4

static struct testreg {
	struct fstr k;
	struct fstr v;
	struct fstr w;
} regs[nregs];

static enum regstore_err getter(void *arg, struct fstr *value)
{
	fstr_copy(value, (const struct fstr *) arg);
	return regstore_err_ok;
}

static enum regstore_err setter(void *arg, const struct fstr *value)
{
	fstr_copy((struct fstr *) arg, value);
	return regstore_err_ok;
}

static void observer(void *arg, const struct fstr *value)
{
	printf(" * Observer: " PRIfs " = " PRIfs "\n", prifs((struct fstr *) arg), prifs(value));
}

static void list_regs()
{
	printf("Listing registers\n");

	for (size_t i = 0; i < nregs; i++) {
		struct testreg *r = &regs[i];
		printf(" * " PRIfs " = " PRIfs "\n", prifs(&r->k), prifs(&r->v));
	}

	printf("\n");
}

static void list_regs_ext(struct regstore *rs)
{
	printf("Listing registers\n");

	struct binary_tree data;
	regstore_list(rs, &data, &rem, true);

	const struct regstore_reginfo *info;
	struct binary_tree_iterator it;
	binary_tree_iter_init(&it, &data, false);
	while ((info = binary_tree_iter_next(&it, NULL))) {
		printf(" * " PRIfs " [%c%c]%s = " PRIfs "\n", prifs(&info->name), info->type & rt_readable ? 'r' : '-', info->type & rt_writeable ? 'w' : '-', info->subscribed ? " [sub]" : "", prifs(&info->value));
	}
	binary_tree_iter_destroy(&it);

	binary_tree_destroy(&data);

	printf("\n");
}

#define testres(i, expr) \
	do { \
		enum regstore_err res = expr; \
		if (res != regstore_err_ok) { \
			log_error("Operation failed on register " PRIfs ": %s, operation=%s", prifs(&regs[i].k), regstore_errstr(res), #expr); \
		} \
	} while (0)

static void init_test_data()
{
	fstr_init_ref(&rem, "test node");

	fstr_init_ref(&regs[0].k, "color");
	fstr_init_ref(&regs[1].k, "size");
	fstr_init_ref(&regs[2].k, "shape");
	fstr_init_ref(&regs[3].k, "count");

	fstr_init_ref(&regs[0].v, "red");
	fstr_init_ref(&regs[1].v, "big");
	fstr_init_ref(&regs[2].v, "round");
	fstr_init_ref(&regs[3].v, "many");

	fstr_init_ref(&regs[0].w, "blue");
	fstr_init_ref(&regs[1].w, "small");
	fstr_init_ref(&regs[2].w, "square");
	fstr_init_ref(&regs[3].w, "few");
}

#define header(s) printf("\x1b[1m" s "\x1b[0m")

static void test_list()
{
	header("List test\n");

	struct regstore rs;

	regstore_init(&rs);

	/* Add registers */
	for (size_t i = 0; i < nregs; i++) {
		struct testreg *r = &regs[i];
		if (!regstore_add(&rs, &r->k, i & 1 ? getter : NULL, &r->v, i & 2 ? setter : NULL, &r->v)) {
			log_error("Failed to create register " PRIfs, prifs(&r->k));
		}
	}

	list_regs_ext(&rs);

	regstore_destroy(&rs);

	printf("\n");
}

static void test_access()
{
	header("Access test\n");

	struct regstore rs;

	regstore_init(&rs);

	/* Add registers */
	for (size_t i = 0; i < nregs; i++) {
		struct testreg *r = &regs[i];
		if (!regstore_add(&rs, &r->k, i & 1 ? getter : NULL, &r->v, i & 2 ? setter : NULL, &r->v)) {
			log_error("Failed to create register " PRIfs, prifs(&r->k));
		}
	}

	list_regs(&rs);

	printf("Reading registers\n");
	for (size_t i = 0; i < nregs; i++) {
		struct fstr v = FSTR_INIT;
		testres(i, regstore_get(&rs, &regs[i].k, &v));
		fstr_destroy(&v);
	}
	printf("\n");

	printf("Writing registers\n");
	for (size_t i = 0; i < nregs; i++) {
		testres(i, regstore_set(&rs, &regs[i].k, &regs[i].w));
	}
	printf("\n");

	list_regs(&rs);

	regstore_destroy(&rs);

	printf("\n");
}

static void test_obs()
{
	header("Observer test\n");

	struct regstore rs;

	regstore_init(&rs);

	/* Add registers */
	for (size_t i = 0; i < nregs; i++) {
		struct testreg *r = &regs[i];
		if (!regstore_add(&rs, &r->k, getter, &r->v, setter, &r->v)) {
			log_error("Failed to create register " PRIfs, prifs(&r->k));
		}
	}

	for (size_t i = 0; i < nregs; i++) {
		struct testreg *r = &regs[i];
		regstore_observe(&rs, &r->k, &rem, observer, &r->k, 1);
	}

	list_regs(&rs);

	/* Test registers */
	for (size_t i = 0; i < nregs; i++) {
		testres(i, regstore_set(&rs, &regs[i].k, &regs[i].w));
		struct fstr v = FSTR_INIT;
		testres(i, regstore_get(&rs, &regs[i].k, &v));
		fstr_destroy(&v);
	}
	printf("\n");

	list_regs(&rs);

	regstore_destroy(&rs);

	fstr_destroy(&rem);

	printf("\n");
}

int main(int argc, char *argv[])
{
	(void) argc;
	(void) argv;

	init_test_data();

	test_list();
	test_access();
	test_obs();

	return 0;
}
#endif
